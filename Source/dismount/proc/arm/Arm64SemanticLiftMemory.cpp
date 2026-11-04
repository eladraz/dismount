/*
 * Arm64SemanticLiftMemory.cpp
 *
 * Implementation file
 *
 * The A64 base lifters of the loads and stores: single registers and pairs
 * (the general purpose and the SIMD&FP registers), the exclusives, the
 * acquire / release forms, the LSE atomics, and the prefetches. See
 * Arm64SemanticInternal.h.
 *
 * The order of a load or a store with a writeback follows the Arm ARM: the
 * address is computed (and the value of the store read) first, then the
 * memory is accessed, then the register is written, and the base register is
 * written last.
 *
 * The exclusive monitor is modeled by the registers ARM64_REG_EXCLUSIVE_*: a
 * load exclusive records the address and the size (non zero), a store
 * exclusive succeeds (writes 0) when the monitor is open and holds its
 * address, writes 1 otherwise, and closes the monitor in both cases.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/dismount.h"
#include "xStl/types.h"
#include "xStl/except/trace.h"
#include "xStl/except/assert.h"
#include "dismount/semantics/SemanticType.h"
#include "dismount/semantics/SemanticOperation.h"
#include "dismount/semantics/SemanticOperand.h"
#include "dismount/semantics/SemanticInstruction.h"
#include "dismount/proc/arm/ArmOpcode.h"
#include "dismount/proc/arm/Arm64Architecture.h"
#include "dismount/proc/arm/ArmSemantics.h"
#include "dismount/proc/arm/Arm64SemanticInternal.h"

using namespace armdis;
using namespace armdis::a64;

typedef SemanticOperation Op;

namespace {

// Return the size in bits of the register operand 'index'
uint getRegisterBits(ArmLiftContext& c, uint index)
{
    return c.getArchitecture()->getRegister(c.getOperand(index).m_register).m_bits;
}

// Add a load with a memory order
SemanticOperand loadOrdered(ArmLiftContext& c, const SemanticOperand& address,
                            const SemanticType& type, Op::MemoryOrder order)
{
    SemanticOperand result = c.newTemporary(type);
    SemanticInstruction instruction(Op::OP_LOAD, type, result, address);
    instruction.setAttributes(SemanticInstruction::ATTRIBUTE_MAY_FAULT);
    instruction.setMemoryOrder(order);
    c.emit(instruction);
    return result;
}

// Add a store with a memory order
void storeOrdered(ArmLiftContext& c, const SemanticOperand& address,
                  const SemanticOperand& value, Op::MemoryOrder order)
{
    SemanticInstruction instruction(Op::OP_STORE, value.getType(),
                                    SemanticOperand(), address, value);
    instruction.setAttributes(SemanticInstruction::ATTRIBUTE_MAY_FAULT);
    instruction.setMemoryOrder(order);
    c.emit(instruction);
}

// Return the address of the memory operand 'index', or of a literal
SemanticOperand getAccessAddress(ArmLiftContext& c, uint index)
{
    const ArmDecodedOperand& operand = c.getOperand(index);
    if (operand.m_kind == ArmDecodedOperand::KIND_BRANCH_TARGET)
        return c.getImmediate(operand.m_immediate, SemanticType::integer(64));
    return c.getAddress(index);
}

// Return 'address' + 'offset'
SemanticOperand addOffset(ArmLiftContext& c, const SemanticOperand& address,
                          uint offset)
{
    if (offset == 0)
        return address;
    return emitImmediate(c, Op::OP_ADD, SemanticType::integer(64), address, offset);
}

// Return the register of a pair after 'first' (x30 is followed by xzr)
uint getPairSecond(uint first)
{
    if (first == ARM64_REG_X0 + 30)
        return ARM64_REG_XZR;
    if (first == ARM64_REG_W0 + 30)
        return ARM64_REG_WZR;
    return first + 1;
}

// Return the value of the register operand 'index' for a store of 'bits'
// bits
SemanticOperand readStoreValue(ArmLiftContext& c, uint index, uint bits)
{
    const ArmDecodedOperand& operand = c.getOperand(index);
    const uint registerBits = getRegisterBits(c, index);
    SemanticOperand value;
    if (c.getArchitecture()->getRegister(operand.m_register).m_isZero)
        value = c.getImmediate(0, SemanticType::integer(registerBits));
    else
        value = c.getRegister(operand.m_register);
    if (bits < registerBits)
        value = c.emit(Op::OP_TRUNCATE, SemanticType::integer(bits), value);
    return value;
}

// Write the loaded value into register operand 'index', extending it to the
// register
void writeLoadValue(ArmLiftContext& c, uint index, const SemanticOperand& value,
                    bool isSigned)
{
    const uint registerBits = getRegisterBits(c, index);
    SemanticOperand extended = value;
    if (value.getType().getTotalBits() < registerBits)
        extended = extend(c, SemanticType::integer(registerBits), value, isSigned);
    c.write(index, extended);
}

/*
 * The single register loads and stores: ldr, str, ldrb, ldrh, ldrsb, ldrsh,
 * ldrsw and their unscaled (ldur) and unprivileged (ldtr) forms. The name is
 * the pattern ld|st, [u|t], r, [b|h|sb|sh|sw]
 */
void liftSingle(ArmLiftContext& c)
{
    const char* name = getMnemonic(c);
    const bool isLoad = name[0] == 'l';
    const char* p = name + 2;
    if ((*p == 'u') || (*p == 't'))
        p++;
    CHECK(*p == 'r');
    p++;
    const uint registerBits = getRegisterBits(c, 0);
    uint bytes = registerBits / 8;
    bool isSigned = false;
    if (strcmp(p, "b") == 0)
        bytes = 1;
    else if (strcmp(p, "h") == 0)
        bytes = 2;
    else if (strcmp(p, "sb") == 0)
    {
        bytes = 1;
        isSigned = true;
    } else if (strcmp(p, "sh") == 0)
    {
        bytes = 2;
        isSigned = true;
    } else if (strcmp(p, "sw") == 0)
    {
        bytes = 4;
        isSigned = true;
    }
    const SemanticType type = SemanticType::integer(bytes * 8);

    const SemanticOperand address = getAccessAddress(c, 1);
    if (isLoad)
    {
        writeLoadValue(c, 0, c.load(address, type), isSigned);
    } else
    {
        c.store(address, readStoreValue(c, 0, bytes * 8));
    }
    if (c.getOperand(1).m_kind == ArmDecodedOperand::KIND_MEMORY)
        c.writeBack(1);
}

// ldp, stp, ldnp, stnp, ldpsw (and ldtp, sttp, ldtnp, sttnp)
void liftPair(ArmLiftContext& c)
{
    const char* name = getMnemonic(c);
    const bool isLoad = name[0] == 'l';
    const bool isSigned = strcmp(name, "ldpsw") == 0;
    const uint registerBits = getRegisterBits(c, 0);
    const uint bytes = isSigned ? 4 : (registerBits / 8);
    const SemanticType type = SemanticType::integer(bytes * 8);

    // FEAT_LRCPC3 ldiapp / stilp: the acquire (RCpc) and release forms
    const bool isOrdered = (strcmp(name, "ldiapp") == 0) || (strcmp(name, "stilp") == 0);
    const Op::MemoryOrder order = !isOrdered ? Op::ORDER_RELAXED :
        (isLoad ? Op::ORDER_ACQUIRE_PROCESSOR_CONSISTENT : Op::ORDER_RELEASE);

    const SemanticOperand address = c.getAddress(2);
    const SemanticOperand second = addOffset(c, address, bytes);
    if (isLoad)
    {
        // Both values are read before either register is written
        SemanticOperand first = loadOrdered(c, address, type, order);
        SemanticOperand next = loadOrdered(c, second, type, order);
        writeLoadValue(c, 0, first, isSigned);
        writeLoadValue(c, 1, next, isSigned);
    } else
    {
        SemanticOperand first = readStoreValue(c, 0, bytes * 8);
        SemanticOperand next = readStoreValue(c, 1, bytes * 8);
        storeOrdered(c, address, first, order);
        storeOrdered(c, second, next, order);
    }
    c.writeBack(2);
}

// prfm, prfum, rprfm: a hint
void liftPrefetch(ArmLiftContext& c)
{
    const uint last = c.getOperandsCount() - 1;
    c.emit(SemanticInstruction(Op::OP_PREFETCH, SemanticType(), SemanticOperand(),
                               getAccessAddress(c, last)));
}

/*
 * The exclusives. The name is the pattern ld|st, [a|l], [t], x, r|p,
 * [b|h]: ldxr, ldaxr, stxr, stlxr, ldxp, ldaxp, stxp, stlxp
 */
void liftExclusive(ArmLiftContext& c)
{
    const char* name = getMnemonic(c);
    const bool isLoad = name[0] == 'l';
    const char* p = name + 2;
    const bool isOrdered = (*p == 'a') || (*p == 'l');
    if (isOrdered)
        p++;
    if (*p == 't')
        p++;
    CHECK(*p == 'x');
    p++;
    const bool isPair = *p == 'p';
    p++;

    // The operands: [status,] Rt, [Rt2,] memory
    const uint first = isLoad ? 0 : 1;
    const uint memoryIndex = first + (isPair ? 2 : 1);
    const uint registerBits = getRegisterBits(c, first);
    uint bytes = registerBits / 8;
    if (strcmp(p, "b") == 0)
        bytes = 1;
    else if (strcmp(p, "h") == 0)
        bytes = 2;
    const SemanticType type = SemanticType::integer(bytes * 8);
    const SemanticType word = SemanticType::integer(64);
    const uint totalBytes = isPair ? (2 * bytes) : bytes;

    const SemanticOperand address = c.getAddress(memoryIndex);
    if (isLoad)
    {
        const Op::MemoryOrder order = isOrdered ? Op::ORDER_ACQUIRE :
                                                  Op::ORDER_RELAXED;
        SemanticOperand firstValue = loadOrdered(c, address, type, order);
        SemanticOperand secondValue;
        if (isPair)
            secondValue = loadOrdered(c, addOffset(c, address, bytes), type,
                                      order);
        // The monitor
        c.move(c.getRegister(ARM64_REG_EXCLUSIVE_ADDRESS), address);
        c.move(c.getRegister(ARM64_REG_EXCLUSIVE_SIZE),
               c.getImmediate(totalBytes, word));
        writeLoadValue(c, 0, firstValue, false);
        if (isPair)
            writeLoadValue(c, 1, secondValue, false);
        return;
    }

    // The store: succeeds only if the monitor is open at this address
    const Op::MemoryOrder order = isOrdered ? Op::ORDER_RELEASE :
                                              Op::ORDER_RELAXED;
    SemanticOperand firstValue = readStoreValue(c, first, bytes * 8);
    SemanticOperand secondValue;
    if (isPair)
        secondValue = readStoreValue(c, first + 1, bytes * 8);
    SemanticOperand isOpen = compare(c, Op::COND_NOT_EQUAL, word,
            c.getRegister(ARM64_REG_EXCLUSIVE_SIZE), c.getImmediate(0, word));
    SemanticOperand isSame = compare(c, Op::COND_EQUAL, word,
            c.getRegister(ARM64_REG_EXCLUSIVE_ADDRESS), address);
    SemanticOperand isPassed = c.emit(Op::OP_AND, SemanticType::integer(1),
                                      isOpen, isSame);
    SemanticOperand failLabel = c.newLabel();
    SemanticOperand endLabel = c.newLabel();
    c.branchIf(c.emit(Op::OP_XOR, SemanticType::integer(1), isPassed,
                      booleanConstant(c, true)), failLabel);
    storeOrdered(c, address, firstValue, order);
    if (isPair)
        storeOrdered(c, addOffset(c, address, bytes), secondValue, order);
    c.write(0, c.getImmediate(0, SemanticType::integer(32)));
    c.branch(endLabel);
    c.placeLabel(failLabel);
    c.write(0, c.getImmediate(1, SemanticType::integer(32)));
    c.placeLabel(endLabel);
    clearExclusiveMonitor(c);
}

/*
 * The acquire and release forms: ldar, stlr, ldlar, stllr, ldapr, ldapur,
 * stlur, with the b, h, sb, sh and sw suffixes
 */
void liftOrdered(ArmLiftContext& c)
{
    static const struct {
        const char* m_prefix;
        bool m_isLoad;
        Op::MemoryOrder m_order;
    } kinds[] = {
        { "ldapur", true,  Op::ORDER_ACQUIRE_PROCESSOR_CONSISTENT },
        { "ldapr",  true,  Op::ORDER_ACQUIRE_PROCESSOR_CONSISTENT },
        { "ldlar",  true,  Op::ORDER_ACQUIRE },
        { "ldar",   true,  Op::ORDER_ACQUIRE },
        { "stllr",  false, Op::ORDER_RELEASE },
        { "stlur",  false, Op::ORDER_RELEASE },
        { "stlr",   false, Op::ORDER_RELEASE }
    };
    const char* name = getMnemonic(c);
    uint kind = 0;
    for (; kind < sizeof(kinds) / sizeof(kinds[0]); kind++)
        if (strncmp(name, kinds[kind].m_prefix, strlen(kinds[kind].m_prefix)) == 0)
            break;
    CHECK(kind < sizeof(kinds) / sizeof(kinds[0]));
    const char* suffix = name + strlen(kinds[kind].m_prefix);

    uint bytes = getRegisterBits(c, 0) / 8;
    bool isSigned = false;
    if (strcmp(suffix, "b") == 0)
        bytes = 1;
    else if (strcmp(suffix, "h") == 0)
        bytes = 2;
    else if (strcmp(suffix, "sb") == 0)
    {
        bytes = 1;
        isSigned = true;
    } else if (strcmp(suffix, "sh") == 0)
    {
        bytes = 2;
        isSigned = true;
    } else if (strcmp(suffix, "sw") == 0)
    {
        bytes = 4;
        isSigned = true;
    }
    const SemanticType type = SemanticType::integer(bytes * 8);
    const SemanticOperand address = c.getAddress(1);
    if (kinds[kind].m_isLoad)
        writeLoadValue(c, 0, loadOrdered(c, address, type, kinds[kind].m_order),
                       isSigned);
    else
        storeOrdered(c, address, readStoreValue(c, 0, bytes * 8),
                     kinds[kind].m_order);
    c.writeBack(1);
}

// The operation of an atomic read-modify-write by the name of its base
struct AtomicBase {
    const char* m_name;
    Op::Operation m_operation;
};

/*
 * The LSE atomics: cas, casp, swp, ld<op>, st<op>, with the acquire (a),
 * release (l) and size (b, h) letters, and the unprivileged forms of FEAT_LSUI
 * (a t anywhere in the letters)
 */
void liftAtomic(ArmLiftContext& c)
{
    // Longer names first: casp before cas
    static const AtomicBase bases[] = {
        { "casp", Op::OP_NOP }, { "cas", Op::OP_NOP }, { "swpp", Op::OP_NOP },
        { "swp", Op::OP_NOP }, { "ldclrp", Op::OP_AND_NOT }, { "ldsetp", Op::OP_OR },
        { "ldadd", Op::OP_ADD }, { "ldclr", Op::OP_AND_NOT },
        { "ldeor", Op::OP_XOR }, { "ldset", Op::OP_OR },
        { "ldsmax", Op::OP_MAXIMUM_SIGNED }, { "ldsmin", Op::OP_MINIMUM_SIGNED },
        { "ldumax", Op::OP_MAXIMUM_UNSIGNED },
        { "ldumin", Op::OP_MINIMUM_UNSIGNED },
        { "ldtadd", Op::OP_ADD }, { "ldtclr", Op::OP_AND_NOT },
        { "ldtset", Op::OP_OR },
        { "stadd", Op::OP_ADD }, { "stclr", Op::OP_AND_NOT },
        { "steor", Op::OP_XOR }, { "stset", Op::OP_OR },
        { "stsmax", Op::OP_MAXIMUM_SIGNED }, { "stsmin", Op::OP_MINIMUM_SIGNED },
        { "stumax", Op::OP_MAXIMUM_UNSIGNED },
        { "stumin", Op::OP_MINIMUM_UNSIGNED }
    };
    const char* name = getMnemonic(c);
    uint index = 0;
    for (; index < sizeof(bases) / sizeof(bases[0]); index++)
        if (strncmp(name, bases[index].m_name, strlen(bases[index].m_name)) == 0)
            break;
    CHECK(index < sizeof(bases) / sizeof(bases[0]));
    const AtomicBase& base = bases[index];
    const bool isStoreForm = base.m_name[0] == 's' && base.m_name[1] == 't';
    const bool isCas = strncmp(base.m_name, "cas", 3) == 0;
    const bool isPair = strcmp(base.m_name, "casp") == 0;
    const bool isPair128 = (strcmp(base.m_name, "swpp") == 0) ||
                           (strcmp(base.m_name, "ldclrp") == 0) ||
                           (strcmp(base.m_name, "ldsetp") == 0);
    const bool isSwap = (strcmp(base.m_name, "swp") == 0) ||
                        (strcmp(base.m_name, "swpp") == 0);

    // The letters after the base: acquire, release, byte, halfword
    bool isAcquire = false, isRelease = false;
    uint bytes = 0;
    for (const char* p = name + strlen(base.m_name); *p != 0; p++)
    {
        switch (*p)
        {
        case 'a': isAcquire = true; break;
        case 'l': isRelease = true; break;
        case 'b': bytes = 1; break;
        case 'h': bytes = 2; break;
        case 't': break;
        default:
            undefined(c);
            return;
        }
    }
    Op::MemoryOrder order = Op::ORDER_RELAXED;
    if (isAcquire && isRelease)
        order = Op::ORDER_ACQUIRE_RELEASE;
    else if (isAcquire)
        order = Op::ORDER_ACQUIRE;
    else if (isRelease)
        order = Op::ORDER_RELEASE;

    // The operands: cas Rs, Rt, [Xn]; ld<op> Rs, Rt, [Xn]; st<op> Rs, [Xn]
    const uint memoryIndex = isStoreForm ? 1 : 2;
    if (bytes == 0)
        bytes = getRegisterBits(c, 0) / 8;
    const uint accessBytes = (isPair || isPair128) ? (bytes * 2) : bytes;
    const SemanticType type = SemanticType::integer(accessBytes * 8);
    const SemanticType half = SemanticType::integer(bytes * 8);
    const SemanticOperand address = c.getAddress(memoryIndex);

    // The first operand as a value of the access size: a pair is the two
    // registers, Rs low
    SemanticOperand source;
    if (isPair128)
    {
        // FEAT_LSE128: the value is the pair Xt:Xt2 (Xt low), read before
        // anything is written
        source = c.emit(Op::OP_CONCAT, type, readStoreValue(c, 0, bytes * 8),
                        readStoreValue(c, 1, bytes * 8));
    } else if (isPair)
    {
        const uint firstRegister = c.getOperand(0).m_register;
        source = c.emit(Op::OP_CONCAT, type,
                        c.getRegister(firstRegister),
                        c.getRegister(getPairSecond(firstRegister)));
    } else
    {
        source = readStoreValue(c, 0, bytes * 8);
    }

    SemanticOperand old = c.newTemporary(type);
    if (isCas)
    {
        SemanticOperand replacement;
        if (isPair)
        {
            const uint secondRegister = c.getOperand(1).m_register;
            replacement = c.emit(Op::OP_CONCAT, type,
                                 c.getRegister(secondRegister),
                                 c.getRegister(getPairSecond(secondRegister)));
        } else
        {
            replacement = readStoreValue(c, 1, bytes * 8);
        }
        SemanticInstruction exchange(Op::OP_ATOMIC_COMPARE_EXCHANGE, type, old,
                                     address, source, replacement);
        exchange.setMemoryOrder(order);
        exchange.setAttributes(SemanticInstruction::ATTRIBUTE_MAY_FAULT);
        c.emit(exchange);
        // The expected register(s) get the old value
        if (isPair)
        {
            const uint firstRegister = c.getOperand(0).m_register;
            if (c.getArchitecture()->getRegister(firstRegister).m_isZero)
                return;
            c.move(c.getRegister(firstRegister),
                   c.emit(Op::OP_TRUNCATE, half, old));
            c.move(c.getRegister(getPairSecond(firstRegister)),
                   c.emit(Op::OP_EXTRACT, half, old,
                          c.getImmediate(bytes * 8, SemanticType::integer(32))));
        } else
        {
            writeLoadValue(c, 0, old, false);
        }
        return;
    }

    if (isSwap)
    {
        SemanticInstruction exchange(Op::OP_ATOMIC_EXCHANGE, type, old, address,
                                     source);
        exchange.setMemoryOrder(order);
        exchange.setAttributes(SemanticInstruction::ATTRIBUTE_MAY_FAULT);
        c.emit(exchange);
    } else
    {
        SemanticInstruction operation(Op::OP_ATOMIC_RMW, type, old, address,
                                      source);
        operation.setCombineOperation(base.m_operation);
        operation.setMemoryOrder(order);
        operation.setAttributes(SemanticInstruction::ATTRIBUTE_MAY_FAULT);
        c.emit(operation);
    }
    if (isPair128)
    {
        // The old value goes to the pair
        c.write(0, c.emit(Op::OP_TRUNCATE, half, old));
        c.write(1, c.emit(Op::OP_EXTRACT, half, old,
                          c.getImmediate(bytes * 8, SemanticType::integer(32))));
        return;
    }
    // The old value goes to Rt (the st<op> forms have none)
    if (!isStoreForm)
        writeLoadValue(c, 1, old, false);
}

// ld64b, st64b: eight doublewords to / from eight consecutive registers
void liftLoadStore64Byte(ArmLiftContext& c)
{
    const bool isLoad = getMnemonic(c)[0] == 'l';
    const SemanticType word = SemanticType::integer(64);
    const SemanticOperand address = c.getAddress(1);
    const uint first = c.getOperand(0).m_register;
    for (uint i = 0; i < 8; i++)
    {
        const SemanticOperand part = addOffset(c, address, 8 * i);
        if (isLoad)
        {
            if (first + i <= ARM64_REG_X30)
                c.move(c.getRegister(first + i), c.load(part, word));
        } else
        {
            c.store(part, (first + i <= ARM64_REG_X30) ?
                          c.getRegister(first + i) : c.getImmediate(0, word));
        }
    }
}

} // namespace

void armdis::liftArm64LoadStore(ArmLiftContext& c)
{
    const char* name = getMnemonic(c);
    if ((strncmp(name, "prf", 3) == 0) || (strncmp(name, "rprf", 4) == 0))
    {
        liftPrefetch(c);
        return;
    }
    if ((strcmp(name, "ld64b") == 0) || (strcmp(name, "st64b") == 0))
    {
        liftLoadStore64Byte(c);
        return;
    }
    if ((strncmp(name, "cas", 3) == 0) || (strncmp(name, "swp", 3) == 0) ||
        (strncmp(name, "ldadd", 5) == 0) || (strncmp(name, "ldclr", 5) == 0) ||
        (strncmp(name, "ldeor", 5) == 0) || (strncmp(name, "ldset", 5) == 0) ||
        (strncmp(name, "ldsm", 4) == 0) || (strncmp(name, "ldum", 4) == 0) ||
        (strncmp(name, "ldtadd", 6) == 0) || (strncmp(name, "ldtclr", 6) == 0) ||
        (strncmp(name, "ldtset", 6) == 0) ||
        (strncmp(name, "stadd", 5) == 0) || (strncmp(name, "stclr", 5) == 0) ||
        (strncmp(name, "steor", 5) == 0) || (strncmp(name, "stset", 5) == 0) ||
        (strncmp(name, "stsm", 4) == 0) || (strncmp(name, "stum", 4) == 0))
    {
        liftAtomic(c);
        return;
    }
    if ((strncmp(name, "ldar", 4) == 0) || (strncmp(name, "stlr", 4) == 0) ||
        (strncmp(name, "ldlar", 5) == 0) || (strncmp(name, "stllr", 5) == 0) ||
        (strncmp(name, "ldapr", 5) == 0) || (strncmp(name, "ldapur", 6) == 0) ||
        (strncmp(name, "stlur", 5) == 0))
    {
        liftOrdered(c);
        return;
    }
    // ldxr, ldaxr, ldxp, stxr, stlxr, ldtxr...
    {
        const char* p = name + 2;
        if ((*p == 'a') || (*p == 'l'))
            p++;
        if (*p == 't')
            p++;
        if ((*p == 'x') && ((p[1] == 'r') || (p[1] == 'p')))
        {
            liftExclusive(c);
            return;
        }
    }
    if ((name[2] == 'p') || (strcmp(name, "ldiapp") == 0) ||
        (strcmp(name, "stilp") == 0) || (strcmp(name, "ldnp") == 0) ||
        (strcmp(name, "stnp") == 0) || (strncmp(name, "ldt", 3) == 0 && name[3] == 'p') ||
        (strncmp(name, "stt", 3) == 0 && name[3] == 'p') ||
        (strncmp(name, "ldtn", 4) == 0) || (strncmp(name, "sttn", 4) == 0))
    {
        liftPair(c);
        return;
    }
    liftSingle(c);
}
