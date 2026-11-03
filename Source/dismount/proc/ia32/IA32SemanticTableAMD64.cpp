/*
 * IA32SemanticTableAMD64.cpp
 *
 * The semantics of the instructions which exist in 64 bit mode only, or which
 * differ there: movsxd, cmpxchg16b, swapgs, syscall, sysret, rdtscp. The
 * table is searched before the IA-32 ones in 64 bit mode, see IA32Semantics.h.
 *
 * The entries of the decoder with a name of three forms (cbw/cwde/cdqe,
 * movd/movd/movq, fxsave/fxsave/fxsave64) are lifted by the IA-32 entry of
 * the shorter name, see IA32SemanticLifter::findEntry().
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
#include "dismount/proc/ia32/AMD64Architecture.h"
#include "dismount/proc/ia32/IA32OperandDecoder.h"
#include "dismount/proc/ia32/IA32Semantics.h"

using namespace ia32dis;

typedef SemanticOperation Op;
typedef OpcodeClassification Cl;
typedef IA32LiftContext Context;

namespace {

// The prefix byte of lock
const uint8 PREFIX_LOCK = 0xF0;

// The vector of the general protection fault
const uint VECTOR_GENERAL_PROTECTION = 13;

const uint32 FZ = SemanticFlags::FLAG_ZERO;

SemanticType integerType(uint bits)
{
    return SemanticType::integer(bits);
}

SemanticOperand immediate(Context& c, int64 value, uint bits)
{
    return c.getImmediate(value, integerType(bits));
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

// Return the value of a register in a temporary
SemanticOperand snapshotRegister(Context& c, uint registerId)
{
    return c.emit(Op::OP_MOVE, integerType(64), c.getRegister(registerId));
}

// movsxd r, r/m32: sign extends into a 64 bit register. Without REX.W the
// destination is 32 bits (a move) or 16 bits (only its low bits are written)
void liftMovsxd(Context& c)
{
    const uint bits = c.getOperand(0).m_bits;
    SemanticOperand source = c.read(1, integerType(32));
    if (bits > 32)
        source = c.emit(Op::OP_SIGN_EXTEND, integerType(bits), source);
    else if (bits < 32)
        source = c.emit(Op::OP_TRUNCATE, integerType(bits), source);
    c.write(0, source);
}

// Call the lifter of the IA-32 mnemonic 'name'
void liftAsIa32(Context& c, const char* name)
{
    for (const IA32SemanticEntry* entry = gIa32SemanticTable;
         entry->m_template.m_name != NULL; entry++)
    {
        if ((strcmp(entry->m_template.m_name, name) == 0) && (entry->m_lift != NULL))
        {
            entry->m_lift(c);
            return;
        }
    }
    CHECK_FAIL();
}

// cmpxchg8b m64 / cmpxchg16b m128 (REX.W): rdx:rax is compared with the
// memory, rcx:rbx is stored if they are equal. The memory of cmpxchg16b must
// be 16 byte aligned (#GP otherwise)
void liftCmpxchg16b(Context& c)
{
    if (c.getOperand(0).m_bits != 128)
    {
        liftAsIa32(c, "cmpxchg8b");
        return;
    }

    const SemanticType type = integerType(128);
    const bool isLocked = IA32OperandDecoder::hasPrefix(c.getInstruction(), PREFIX_LOCK);
    SemanticOperand address = c.getAddress(0);
    SemanticOperand ok = c.newLabel();
    c.branchIf(compare(c, Op::COND_EQUAL,
                       c.emit(Op::OP_AND, integerType(64), address, immediate(c, 15, 64)),
                       immediate(c, 0, 64)), ok);
    c.trap(Op::TRAP_FAULT, VECTOR_GENERAL_PROTECTION);
    c.placeLabel(ok);

    SemanticOperand expected = c.emit(Op::OP_CONCAT, type,
                                      c.getRegister(AMD64_REG_RAX),
                                      c.getRegister(AMD64_REG_RDX));
    SemanticOperand source = c.emit(Op::OP_CONCAT, type,
                                    c.getRegister(AMD64_REG_RBX),
                                    c.getRegister(AMD64_REG_RCX));
    SemanticOperand old;
    if (isLocked)
    {
        old = c.newTemporary(type);
        SemanticInstruction exchange(Op::OP_ATOMIC_COMPARE_EXCHANGE, type, old,
                                     address, expected, source);
        exchange.setMemoryOrder(Op::ORDER_SEQUENTIAL);
        exchange.setAttributes(SemanticInstruction::ATTRIBUTE_MAY_FAULT);
        c.emit(exchange);
    } else
    {
        old = c.load(address, type);
    }
    // The comparisons are on numbers of up to 64 bits: the halves
    SemanticOperand low = c.emit(Op::OP_TRUNCATE, integerType(64), old);
    SemanticOperand high = c.emit(Op::OP_EXTRACT, integerType(64), old,
                                  immediate(c, 64, 32));
    SemanticOperand isEqual = c.emit(Op::OP_AND, integerType(1),
        compare(c, Op::COND_EQUAL, low, snapshotRegister(c, AMD64_REG_RAX)),
        compare(c, Op::COND_EQUAL, high, snapshotRegister(c, AMD64_REG_RDX)));
    if (!isLocked)
        c.store(address, c.emit(Op::OP_SELECT, type, isEqual, source, old));
    c.setFlags(FZ, isEqual);
    // The old value goes to rdx:rax if they were different
    SemanticOperand skip = c.newLabel();
    c.branchIf(isEqual, skip);
    c.move(c.getRegister(AMD64_REG_RAX), low);
    c.move(c.getRegister(AMD64_REG_RDX), high);
    c.placeLabel(skip);
}

// swapgs: the gs base and the other gs base (IA32_KERNEL_GS_BASE) are exchanged
void liftSwapgs(Context& c)
{
    SemanticOperand user = c.emit(Op::OP_MOVE, integerType(64),
                                  c.getRegister(IA32_REG_GS_BASE));
    c.move(c.getRegister(IA32_REG_GS_BASE), c.getRegister(AMD64_REG_KERNEL_GS_BASE));
    c.move(c.getRegister(AMD64_REG_KERNEL_GS_BASE), user);
}

// syscall and sysret: a system call trap. The operating system does what the
// instruction does to rcx, r11 and rip
void liftSystemCall(Context& c)
{
    c.trap(Op::TRAP_SYSTEM_CALL, 0);
}

// rdtscp: a helper like rdtsc
void liftRdtscp(Context& c)
{
    c.emit(SemanticInstruction(Op::OP_HELPER, SemanticType(), SemanticOperand(),
                               immediate(c, IA32_HELPER_RDTSCP, 32)));
}

} // namespace

/////////////////////////////////////////////////////////////////////////////
// The table
/////////////////////////////////////////////////////////////////////////////

namespace ia32dis {

namespace {

const uint8 CAT_MOVE      = Cl::CATEGORY_CONVERT;
const uint8 CAT_ATOMIC    = Cl::CATEGORY_ATOMIC;
const uint8 CAT_SYSTEM    = Cl::CATEGORY_SYSTEM;
const uint32 U_ALU        = Cl::UNIT_INTEGER_ALU;
const uint32 U_MEMORY     = Cl::UNIT_LOAD | Cl::UNIT_STORE;
const uint32 U_SYSTEM     = Cl::UNIT_SYSTEM;
const uint32 P_READ       = Cl::PROPERTY_READS_MEMORY;
const uint32 P_READ_WRITE = Cl::PROPERTY_READS_MEMORY | Cl::PROPERTY_WRITES_MEMORY;
const uint32 P_IMPLICIT   = Cl::PROPERTY_IMPLICIT_OPERANDS;
const uint32 P_HELPER     = Cl::PROPERTY_NEEDS_HELPER;
const uint32 P_NONE       = Cl::PROPERTY_NONE;

} // namespace

// An entry of a hand written lifter
#define LIFTER(name, lift, category, units, properties, read, written, undefined) \
    { IA32SemanticEntry::ANY_OPERANDS_COUNT, \
      { name, NULL, 0, category, units, properties, read, written, undefined }, \
      lift }

const IA32SemanticEntry gAmd64SemanticTable[] = {
    // Name          Lifter           Category    Units              Properties   Flags read, written, undefined
    LIFTER("movsxd", liftMovsxd,      CAT_MOVE,   U_ALU | U_MEMORY,  P_READ,      0, 0, 0),
    LIFTER("cmpxchg8b/cmpxchg8b/cmpxchg16b", liftCmpxchg16b, CAT_ATOMIC,
           U_ALU | U_MEMORY, P_READ_WRITE | P_IMPLICIT, 0, FZ, 0),
    LIFTER("swapgs",   liftSwapgs,    CAT_SYSTEM, U_SYSTEM,          P_IMPLICIT,  0, 0, 0),
    LIFTER("syscall",  liftSystemCall, CAT_SYSTEM, U_SYSTEM,         P_IMPLICIT,  0, 0, 0),
    LIFTER("sysret",   liftSystemCall, CAT_SYSTEM, U_SYSTEM,         P_IMPLICIT,  0, 0, 0),
    LIFTER("rdtscp",   liftRdtscp,    CAT_SYSTEM, U_SYSTEM,          P_HELPER | P_IMPLICIT, 0, 0, 0),

    // End marker
    { 0, { NULL, NULL, 0, 0, 0, 0, 0, 0, 0 }, NULL }
};

#undef LIFTER

} // namespace ia32dis
