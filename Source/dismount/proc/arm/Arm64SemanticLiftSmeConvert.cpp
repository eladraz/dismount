/*
 * Arm64SemanticLiftSmeConvert.cpp
 *
 * Implementation file
 *
 * The SME2 instructions which permute, widen or narrow the vectors of a group: zip, uzp, sunpk,
 * uunpk, the conversions between integers and floating point, fcvt / fcvtn / bfcvt / bfcvtn,
 * the saturating narrows (sqcvt..., sqrshr...), the rounding to integral (see
 * Arm64SemanticSme.h).
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/dismount.h"
#include <string.h>
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
#include "dismount/proc/arm/Arm64SemanticSme.h"

using namespace armdis;

typedef SemanticOperation Op;
typedef SmeLift::Val Val;

namespace {

uint log2Of(uint value)
{
    uint r = 0;
    while ((1u << r) < value)
        r++;
    return r;
}

// zip / uzp of two or four registers, element by element
void liftZipUzp(SmeLift& m, bool isZip)
{
    const uint nreg = m.groupCount(0);
    const uint esize = m.elementBits(0);
    const uint L = m.lanes(esize);
    m.begin(true, false);
    Val sources[4];
    if (m.operand(1).m_kind == ArmDecodedOperand::KIND_REGISTER_LIST)
    {
        for (uint r = 0; r < nreg; r++)
            sources[r] = m.zv(m.groupRegister(1, r), esize);
    } else
    {
        sources[0] = m.zv(m.groupRegister(1, 0), esize);
        sources[1] = m.zv(m.groupRegister(2, 0), esize);
    }
    Val results[4];
    for (uint d = 0; d < nreg; d++)
    {
        Val out = m.zeroVec(esize);
        for (uint j = 0; j < L; j++)
        {
            uint source, index;
            if (isZip)
            {
                source = j % nreg;
                index = d * (L / nreg) + j / nreg;
            } else
            {
                const uint flat = nreg * j + d;
                source = flat / L;
                index = flat % L;
            }
            out = m.setLane(out, m.lane(sources[source], index), j);
        }
        results[d] = out;
    }
    for (uint d = 0; d < nreg; d++)
        m.setZ(m.groupRegister(0, d), results[d]);
    m.end();
}

// sunpk / uunpk: the low and the high half of every source, extended
void liftUnpack(SmeLift& m, bool isSigned)
{
    const uint nreg = m.groupCount(0);
    const uint wide = m.elementBits(0);
    const uint narrow = m.elementBits(1);
    const uint nsources = m.groupCount(1);
    const SemanticType half = SemanticType::integer(m.svl() / 2);
    m.begin(true, false);
    Val results[4];
    for (uint k = 0; k < nsources; k++)
    {
        Val bits = m.z(m.groupRegister(1, k));
        for (uint h = 0; h < 2; h++)
        {
            Val part = (h == 0) ? m.c().emit(Op::OP_TRUNCATE, half, bits)
                                : m.c().emit(Op::OP_EXTRACT, half, bits, m.imm(m.svl() / 2, 32));
            Val narrowVector = m.s().cast(SimdLift::V(narrow, m.lanes(wide)), part);
            results[2 * k + h] = m.extendLanes(narrowVector, wide, isSigned);
        }
    }
    for (uint d = 0; d < nreg; d++)
        m.setZ(m.groupRegister(0, d), results[d]);
    m.end();
}

// The sources narrowed and concatenated: mode 0 truncates, 1 saturates the signed values to the
// signed range, 2 saturates the signed values to the unsigned range
Val narrowSources(SmeLift& m, const Val* sources, uint count, uint wide, uint narrow, uint mode)
{
    const uint L = m.lanes(wide);
    const SemanticType pairType = SimdLift::V(narrow, 2 * L);
    Val parts[2];
    for (uint k = 0; k < count; k += 2)
    {
        Val destination = m.c().newTemporary(pairType);
        SemanticInstruction instruction(Op::OP_NARROW, pairType, destination, sources[k],
                                        sources[k + 1]);
        instruction.setAttributes(mode == 1 ? SemanticInstruction::ATTRIBUTE_SATURATE_SIGNED :
                                  mode == 2 ? SemanticInstruction::ATTRIBUTE_SATURATE_UNSIGNED : 0);
        m.c().emit(instruction);
        parts[k / 2] = destination;
    }
    if (count == 2)
        return m.asBits(parts[0]);
    Val low = m.s().cast(SemanticType::integer(m.svl() / 2), parts[0]);
    Val high = m.s().cast(SemanticType::integer(m.svl() / 2), parts[1]);
    return m.c().emit(Op::OP_CONCAT, SemanticType::integer(m.svl()), low, high);
}

// out[count * i + k] = in[k * lanes + i] of the narrowed concatenation (the interleaving forms)
Val interleaveNarrowed(SmeLift& m, const Val& concatenated, uint count, uint narrow)
{
    const uint n = m.lanes(narrow);
    const uint L = n / count;
    const SemanticType type = m.vecType(narrow);
    ArmLiftContext& c = m.c();
    Val j = c.emit(Op::OP_LANE_INDEX, type, m.imm(0, narrow), m.imm(1, narrow));
    Val k = c.emit(Op::OP_AND, type, j, m.s().splat(type, count - 1));
    Val i = c.emit(Op::OP_SHIFT_RIGHT_LOGICAL, type, j, m.s().splat(type, log2Of(count)));
    Val index = c.emit(Op::OP_ADD, type,
                       c.emit(Op::OP_SHIFT_LEFT, type, k, m.s().splat(type, log2Of(L))), i);
    Val v = m.asVec(concatenated, narrow);
    return c.emit(Op::OP_SHUFFLE, type, v, v, index);
}

// sqcvt sqcvtn sqcvtu sqcvtun uqcvt uqcvtn, sqrshr sqrshrn sqrshru sqrshrun uqrshr uqrshrn
void liftNarrowSaturate(SmeLift& m)
{
    const char* name = m.name();
    const bool isShift = strstr(name, "rshr") != NULL;
    const bool unsignedSource = (name[0] == 'u');
    bool interleaved, unsignedResult;
    if (isShift)
    {
        interleaved = (strcmp(name, "sqrshrn") == 0) || (strcmp(name, "sqrshrun") == 0) ||
                      (strcmp(name, "uqrshrn") == 0);
        unsignedResult = (strcmp(name, "sqrshru") == 0) || (strcmp(name, "sqrshrun") == 0);
    } else
    {
        interleaved = (strcmp(name, "sqcvtn") == 0) || (strcmp(name, "sqcvtun") == 0) ||
                      (strcmp(name, "uqcvtn") == 0);
        unsignedResult = (strcmp(name, "sqcvtu") == 0) || (strcmp(name, "sqcvtun") == 0);
    }
    const uint narrow = m.elementBits(0);
    const uint wide = m.elementBits(1);
    const uint count = m.groupCount(1);
    const SemanticType type = m.vecType(wide);
    ArmLiftContext& c = m.c();
    m.begin(true, false);
    Val sources[4];
    for (uint k = 0; k < count; k++)
    {
        Val v = m.zv(m.groupRegister(1, k), wide);
        if (isShift)
        {
            Val destination = c.newTemporary(type);
            SemanticInstruction shift(Op::OP_SHIFT_RIGHT_ROUNDING, type, destination, v,
                                      m.s().splat(type, m.s().imm(m.operand(2).m_immediate,
                                                                  SemanticType::integer(wide))));
            shift.setBehavior(unsignedSource ? Op::BEHAVIOR_UNSIGNED : Op::BEHAVIOR_SIGNED);
            c.emit(shift);
            v = destination;
        }
        if (unsignedSource)
        {
            // the unsigned values are clamped to the range of the result before the narrowing
            const int64 maximum = (int64)((1ull << narrow) - 1);
            v = c.emit(Op::OP_MINIMUM_UNSIGNED, type, v, m.s().splat(type, m.imm(maximum, wide)));
        }
        sources[k] = v;
    }
    const uint mode = unsignedSource ? 0 : (unsignedResult ? 2 : 1);
    Val result = narrowSources(m, sources, count, wide, narrow, mode);
    if (interleaved)
        result = interleaveNarrowed(m, result, count, narrow);
    m.setZ(m.zNumber(0), result);
    m.end();
}

// frintn frintp frintm frinta, scvtf ucvtf, fcvtzs fcvtzu: elementwise on a group
void liftFloatGroup(SmeLift& m)
{
    const uint nreg = m.groupCount(0);
    const uint esize = m.elementBits(0);
    const char* name = m.name();
    m.begin(true, false);
    Val results[4];
    for (uint r = 0; r < nreg; r++)
    {
        Val v = m.zv(m.groupRegister(1, r), esize);
        if (name[0] == 'f' && name[1] == 'r')
        {
            const char letter = name[5];
            Op::RoundingMode mode = letter == 'n' ? Op::ROUND_NEAREST_EVEN :
                                    letter == 'p' ? Op::ROUND_TOWARD_POSITIVE :
                                    letter == 'm' ? Op::ROUND_TOWARD_NEGATIVE : Op::ROUND_NEAREST_AWAY;
            results[r] = m.s().fp(Op::OP_FLOAT_ROUND, v, Val(), Val(), Op::BEHAVIOR_DEFAULT, mode,
                                  Op::FLOAT_ROUND_SUPPRESS_INEXACT);
        } else if (name[0] == 's' || name[0] == 'u')
        {
            results[r] = m.s().integerToFp(v, esize, name[0] == 's', 0);
        } else
        {
            results[r] = m.s().fpToInteger(v, esize, name[5] == 's', Op::ROUND_TOWARD_ZERO, 0);
        }
    }
    for (uint r = 0; r < nreg; r++)
        m.setZ(m.groupRegister(0, r), results[r]);
    m.end();
}

// fcvt fcvtn bfcvt bfcvtn: two single precision vectors narrowed to half precision / BFloat16
void liftFloatNarrow(SmeLift& m)
{
    const char* name = m.name();
    const bool isBf = name[0] == 'b';
    const bool interleaved = name[strlen(name) - 1] == 'n';
    const uint count = m.groupCount(1);
    const uint wide = m.elementBits(1);
    const uint narrow = m.elementBits(0);
    const uint L = m.lanes(wide);
    m.begin(true, false);
    Val halves[2];
    for (uint k = 0; k < count; k++)
    {
        Val v = m.zv(m.groupRegister(1, k), wide);
        // (the half precision of the SME conversions is the IEEE one, whatever FPCR.AHP is)
        halves[k] = m.fpConvert(v, SemanticType::FLOAT_SINGLE,
                                isBf ? SemanticType::FLOAT_BFLOAT16 : SemanticType::FLOAT_HALF, L,
                                false);
    }
    Val low = m.s().cast(SemanticType::integer(L * narrow), halves[0]);
    Val high = m.s().cast(SemanticType::integer(L * narrow), halves[1]);
    Val result = m.c().emit(Op::OP_CONCAT, SemanticType::integer(m.svl()), low, high);
    if (interleaved)
        result = interleaveNarrowed(m, result, count, narrow);
    m.setZ(m.zNumber(0), result);
    m.end();
}

// luti2 / luti4: the elements of the destination registers are entries of zt0 (the low esize bits
// of the 32 bit slot) selected by the 2 / 4 bit indices of a source register, the group of
// indices given by the immediate
void liftLookup(SmeLift& m)
{
    ArmLiftContext& c = m.c();
    const uint bits = m.is("luti2") ? 2 : 4;
    const uint nreg = m.groupCount(0);
    const uint esize = m.elementBits(0);
    const uint n = m.lanes(esize);
    const ArmDecodedOperand& source = m.operand(2);
    const uint groups = esize / (nreg * bits) ? esize / (nreg * bits) : 1;
    const uint imm = (uint)source.m_elementIndex % groups;
    m.begin(true, true);
    Val indices = m.z(source.m_register - ARM64_REG_Z0);
    Val table = c.getRegister(ARM64_REG_ZT0);
    Val results[4];
    for (uint r = 0; r < nreg; r++)
    {
        Val out = m.zeroVec(esize);
        for (uint i = 0; i < n; i++)
        {
            const uint position = nreg * n * imm + r * n + i;
            Val index = c.emit(Op::OP_EXTRACT, SemanticType::integer(bits), indices,
                               m.imm(position * bits, 32));
            Val wide = c.emit(Op::OP_ZERO_EXTEND, SemanticType::integer(32), index);
            Val offset = c.emit(Op::OP_SHIFT_LEFT, SemanticType::integer(32), wide, m.imm(5, 32));
            Val entry = c.emit(Op::OP_EXTRACT, SemanticType::integer(esize), table, offset);
            out = m.setLane(out, entry, i);
        }
        results[r] = out;
    }
    for (uint r = 0; r < nreg; r++)
        m.setZ(m.groupRegister(0, r), results[r]);
    m.end();
}

// fcvt / fcvtl {z.s, z.s}, z.h (FEAT_SME_F16F16): the low and the high half (fcvt) or the even and the
// odd elements (fcvtl) of the half precision source widened to single precision
void liftFloatWiden(SmeLift& m)
{
    const bool isInterleaved = m.is("fcvtl");
    const uint L = m.lanes(32);
    m.begin(true, false);
    Val source = m.zv(m.zNumber(1), 16);
    Val results[2];
    for (uint k = 0; k < 2; k++)
    {
        Val part;
        if (isInterleaved)
        {
            part = m.deinterleave(source, k);
        } else
        {
            const SemanticType half = SemanticType::integer(m.svl() / 2);
            Val bits = m.asBits(source);
            Val piece = (k == 0) ? m.c().emit(Op::OP_TRUNCATE, half, bits)
                                 : m.c().emit(Op::OP_EXTRACT, half, bits, m.imm(m.svl() / 2, 32));
            part = m.s().cast(SimdLift::V(16, L), piece);
        }
        results[k] = m.fpConvert(part, SemanticType::FLOAT_HALF, SemanticType::FLOAT_SINGLE, L,
                                 false);
    }
    for (uint k = 0; k < 2; k++)
        m.setZ(m.groupRegister(0, k), results[k]);
    m.end();
}

} // namespace

bool armdis::liftSmeConvert(SmeLift& m)
{
    if ((m.is("luti2") || m.is("luti4")) && (m.count() == 3) &&
        (m.operand(1).m_register == ARM64_REG_ZT0) && (m.operand(2).m_elementIndex >= 0) &&
        (m.operand(2).m_kind == ArmDecodedOperand::KIND_REGISTER))
    {
        liftLookup(m);
        return true;
    }
    if (m.count() < 2)
        return false;
    const ArmDecodedOperand& o0 = m.operand(0);
    const ArmDecodedOperand& o1 = m.operand(1);
    const bool group0 = o0.m_kind == ArmDecodedOperand::KIND_REGISTER_LIST;
    const bool group1 = o1.m_kind == ArmDecodedOperand::KIND_REGISTER_LIST;
    if (m.is("zip") || m.is("uzp"))
    {
        if (!group0)
            return false;
        liftZipUzp(m, m.is("zip"));
        return true;
    }
    if (m.is("sunpk") || m.is("uunpk"))
    {
        if (!group0)
            return false;
        liftUnpack(m, m.is("sunpk"));
        return true;
    }
    if (m.is("sqcvt") || m.is("sqcvtn") || m.is("sqcvtu") || m.is("sqcvtun") || m.is("uqcvt") ||
        m.is("uqcvtn") || m.is("sqrshr") || m.is("sqrshrn") || m.is("sqrshru") ||
        m.is("sqrshrun") || m.is("uqrshr") || m.is("uqrshrn"))
    {
        if (!group1)
            return false;
        liftNarrowSaturate(m);
        return true;
    }
    if (m.is("frinta") || m.is("frintm") || m.is("frintn") || m.is("frintp") || m.is("scvtf") ||
        m.is("ucvtf") || m.is("fcvtzs") || m.is("fcvtzu"))
    {
        if (!group0 || !group1)
            return false;
        liftFloatGroup(m);
        return true;
    }
    if ((m.is("fcvt") || m.is("fcvtl")) && group0 && !group1 && (m.count() == 2) &&
        (m.groupCount(0) == 2) && (m.elementBits(0) == 32) && (m.elementBits(1) == 16))
    {
        liftFloatWiden(m);
        return true;
    }
    if (m.is("fcvt") || m.is("fcvtn") || m.is("bfcvt") || m.is("bfcvtn"))
    {
        // z.h, {z.s, z.s}: the single precision pair
        if (group0 || !group1 || (m.elementBits(1) != 32) || (m.elementBits(0) != 16))
            return false;
        liftFloatNarrow(m);
        return true;
    }
    return false;
}
