/*
 * RiscvSemanticLiftVectorMemory.cpp
 *
 * Implementation file
 *
 * The vector loads and stores: unit stride, strided, indexed (ordered and
 * unordered, which are the same in this model), the segment forms (nf 2-8),
 * the fault only first loads, the whole register loads and stores and the
 * mask loads and stores (vlm.v, vsm.v).
 *
 * Every access is an element loop (RvvLift::beginLoop()): the elements
 * vstart .. vl - 1 in order, and the loop counter is vstart itself, so an
 * access that faults leaves vstart at the faulting element (the elements
 * before it are done, the destination elements of the segment fields loaded
 * before the fault in the same element are written, as the reference
 * processors do). A masked element that v0 deselects has no access. The
 * fault only first loads trim vl instead of faulting on any element but the
 * first (index 0): OP_PROBE_ACCESS of every field of the element says
 * whether the segment can be read, and vl = the element index when it can't.
 * A fault only first segment is atomic (all its fields are loaded before the
 * first is written, as QEMU does; Spike writes the fields before the one that
 * faults). The other segment accesses are field by field.
 *
 * The parameter of the entries is built by RVV_MEMORY(): the kind, store,
 * fault only first, the number of fields and the element width.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/dismount.h"
#include "xStl/types.h"
#include "xStl/except/assert.h"
#include "dismount/proc/riscv/RiscvOpcode.h"
#include "dismount/proc/riscv/RiscvArchitecture.h"
#include "dismount/proc/riscv/RiscvSemantics.h"
#include "dismount/proc/riscv/RiscvSemanticInternal.h"
#include "dismount/proc/riscv/RiscvSemanticVector.h"

namespace riscvdis {

typedef SemanticOperation Op;

namespace {

// The decoded parameter
struct MemoryAccess {
    uint m_kind;
    bool m_isStore;
    bool m_isFaultFirst;
    uint m_fields;
    // The element width in bits (of the index for an indexed access)
    uint m_eew;
};

MemoryAccess decodeParameter(uint64 parameter)
{
    MemoryAccess access;
    access.m_kind = (uint)(parameter & 7);
    access.m_isStore = ((parameter >> 4) & 1) != 0;
    access.m_isFaultFirst = ((parameter >> 5) & 1) != 0;
    access.m_fields = (uint)((parameter >> 8) & 7) + 1;
    access.m_eew = (uint)8 << ((parameter >> 12) & 3);
    return access;
}

bool isIndexed(const MemoryAccess& access)
{
    return (access.m_kind == RVV_MEM_INDEXED_ORDERED) ||
           (access.m_kind == RVV_MEM_INDEXED_UNORDERED);
}

/*
 * The loop of one access.
 *
 * dataEew        - The width of the data elements
 * fields         - The number of fields of a segment
 * fieldRegisters - The number of registers of the data group of one field
 * limit          - The number of elements (vl, or the size of the whole
 *                  register access)
 * isMasked       - v0 selects the elements
 */
void emitLoop(RvvLift& lift, const MemoryAccess& access, uint dataEew,
              uint fields, uint fieldRegisters, const SemanticOperand& limit,
              bool isMasked)
{
    const SemanticType booleanType = lift.integerType(1);
    const SemanticType dataType = lift.integerType(dataEew);
    const uint bytes = dataEew / 8;
    const uint vd = lift.vd();

    // The operands, read before the loop (the registers don't change in it)
    SemanticOperand base = lift.readX(lift.rs1());
    SemanticOperand stride;
    if (access.m_kind == RVV_MEM_STRIDED)
        stride = lift.readX(lift.field(20, 5));

    SemanticOperand i = lift.beginLoop(limit);
    if (isMasked)
    {
        SemanticOperand bit = lift.maskBit(i);
        lift.skipElementIf(lift.compare(Op::COND_EQUAL, booleanType, bit,
                                        lift.imm(0, booleanType)));
    }

    // The address of the first field of the element
    SemanticOperand elementAddress;
    switch (access.m_kind)
    {
    case RVV_MEM_STRIDED:
        elementAddress = lift.emit(Op::OP_ADD, lift.xlenType(), base,
                lift.emit(Op::OP_MULTIPLY, lift.xlenType(), i, stride));
        break;
    case RVV_MEM_INDEXED_ORDERED:
    case RVV_MEM_INDEXED_UNORDERED:
    {
        SemanticOperand index = lift.readElement(lift.field(20, 5),
                                                 access.m_eew, i);
        elementAddress = lift.emit(Op::OP_ADD, lift.xlenType(), base,
                                   lift.zeroExtend(index, lift.xlen()));
        break;
    }
    default:
        // The elements are consecutive: (i * fields) * bytes
        elementAddress = lift.emit(Op::OP_ADD, lift.xlenType(), base,
                lift.emit(Op::OP_MULTIPLY, lift.xlenType(), i,
                          lift.immX(fields * bytes)));
        break;
    }
    // (the address of a field)
    SemanticOperand addresses[8];
    for (uint f = 0; f < fields; f++)
        addresses[f] = (f == 0) ? elementAddress :
                lift.emit(Op::OP_ADD, lift.xlenType(), elementAddress,
                          lift.immX(f * bytes));

    SemanticOperand failLabel;
    if (access.m_isFaultFirst)
    {
        // The first element faults normally; later ones trim vl
        SemanticOperand loadLabel = lift.newLabel();
        failLabel = lift.newLabel();
        lift.branchIf(lift.compare(Op::COND_EQUAL, lift.xlenType(), i,
                                   lift.immX(0)), loadLabel);
        for (uint f = 0; f < fields; f++)
        {
            SemanticOperand probe = lift.context().newTemporary(booleanType);
            SemanticInstruction instruction(Op::OP_PROBE_ACCESS, dataType,
                                            probe, addresses[f]);
            lift.context().emit(instruction);
            lift.branchIf(lift.compare(Op::COND_EQUAL, booleanType, probe,
                                       lift.imm(0, booleanType)), failLabel);
        }
        lift.placeLabel(loadLabel);
    }

    if (access.m_isFaultFirst)
    {
        // The segment is atomic: every field is loaded (and may fault) before
        // the first one is written, so a fault of a later field leaves the
        // destination as it was (QEMU; Spike writes the fields before it)
        SemanticOperand values[8];
        for (uint f = 0; f < fields; f++)
            values[f] = lift.context().load(addresses[f], dataType);
        for (uint f = 0; f < fields; f++)
            lift.writeElement(vd + f * fieldRegisters, dataEew, i, values[f]);
    } else
    {
        for (uint f = 0; f < fields; f++)
        {
            const uint registerNumber = vd + f * fieldRegisters;
            if (access.m_isStore)
            {
                SemanticOperand value = lift.readElement(registerNumber, dataEew, i);
                lift.context().store(addresses[f], value);
            } else
            {
                SemanticOperand value = lift.context().load(addresses[f], dataType);
                lift.writeElement(registerNumber, dataEew, i, value);
            }
        }
    }
    if (access.m_isFaultFirst)
    {
        lift.skipElement();
        lift.placeLabel(failLabel);
        lift.move(lift.vlRegister(), i);
        lift.leaveLoop();
    }
    lift.endLoop();
}

// The body of the loads and stores that depend on vtype
void accessBody(RvvLift& lift, const RvvType& type)
{
    const MemoryAccess access = decodeParameter(lift.context().getParameter());
    const uint fields = access.m_fields;
    const bool isMasked = lift.isMasked();
    const uint vd = lift.vd();

    if (!isIndexed(access))
    {
        // EEW / SEW * LMUL registers per field
        const RvvGroup group = RvvLift::makeGroup(type, vd, access.m_eew);
        const uint registers = group.registers();
        bool isLegal = RvvLift::isGroupLegal(group) &&
                       (fields * registers <= 8) &&
                       (vd + fields * registers <= 32);
        // A masked load can't write v0
        if (!access.m_isStore && isMasked && (vd == 0))
            isLegal = false;
        if (!isLegal)
        {
            lift.trapIllegal();
            return;
        }
        emitLoop(lift, access, access.m_eew, fields, registers, lift.vl(),
                 isMasked);
        return;
    }

    // Indexed: the data is SEW, the index is EEW
    const RvvGroup data = RvvLift::makeGroupLmul(type, vd, type.m_sew,
                                                 type.m_lmulLog2);
    const RvvGroup index = RvvLift::makeGroup(type, lift.field(20, 5),
                                              access.m_eew);
    const uint flmul = data.registers();
    bool isLegal = (access.m_eew <= lift.xlen()) &&
                   RvvLift::isGroupLegal(data) && RvvLift::isGroupLegal(index) &&
                   (fields * flmul <= 8) && (vd + fields * flmul <= 32);
    if (isLegal && !access.m_isStore)
    {
        if (isMasked && (vd == 0))
            isLegal = false;
        for (uint f = 0; isLegal && (f < fields); f++)
        {
            RvvGroup segment = data;
            segment.m_register = vd + f * flmul;
            if (access.m_eew > type.m_sew)
            {
                if ((segment.m_register != index.m_register) &&
                    RvvLift::isOverlapped(segment, index))
                    isLegal = false;
            } else if (access.m_eew < type.m_sew)
            {
                if (index.m_emulLog2 < 0)
                    isLegal = !RvvLift::isOverlapped(segment, index);
                else
                    isLegal = !RvvLift::isOverlappedWiden(segment, index);
            }
            if (isLegal && (fields >= 2) &&
                RvvLift::isOverlapped(segment, index))
                isLegal = false;
        }
    }
    if (!isLegal)
    {
        lift.trapIllegal();
        return;
    }
    emitLoop(lift, access, type.m_sew, fields, flmul, lift.vl(), isMasked);
}

} // namespace

void liftVectorMemory(RiscvLiftContext& context)
{
    RvvLift lift(context);
    const MemoryAccess access = decodeParameter(context.getParameter());
    if (access.m_kind == RVV_MEM_WHOLE)
    {
        // vl<n>re<w>.v, vs<n>r.v: n registers, no vtype, no vl, no mask
        lift.begin(false);
        const uint registers = access.m_fields;
        if ((lift.vd() % registers) != 0)
        {
            lift.trapIllegal();
        } else
        {
            const uint count = registers * lift.vlen() / access.m_eew;
            emitLoop(lift, access, access.m_eew, 1, registers,
                     lift.immX(count), false);
        }
        lift.placeLabel(lift.endLabel());
        lift.finish();
        return;
    }
    if (access.m_kind == RVV_MEM_MASK)
    {
        // vlm.v, vsm.v: ceil(vl / 8) bytes
        lift.begin(true);
        SemanticOperand count = lift.emit(Op::OP_SHIFT_RIGHT_LOGICAL,
                lift.xlenType(),
                lift.emit(Op::OP_ADD, lift.xlenType(), lift.vl(), lift.immX(7)),
                lift.immX(3));
        emitLoop(lift, access, 8, 1, 1, count, false);
        lift.placeLabel(lift.endLabel());
        lift.finish();
        return;
    }
    lift.dispatch(accessBody);
    lift.finish();
}

} // namespace riscvdis
