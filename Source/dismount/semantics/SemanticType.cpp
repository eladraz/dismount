/*
 * SemanticType.cpp
 *
 * Implementation file
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/dismount.h"
#include "xStl/types.h"
#include "xStl/except/trace.h"
#include "dismount/semantics/SemanticType.h"

SemanticType::SemanticType() :
    m_kind(KIND_VOID),
    m_floatFormat(FLOAT_NONE),
    m_isScalable(false),
    m_elementBits(0),
    m_lanes(1)
{
}

SemanticType::SemanticType(Kind kind,
                           uint elementBits,
                           uint lanes,
                           FloatFormat format) :
    m_kind((uint8)kind),
    m_floatFormat((uint8)format),
    m_isScalable(false),
    m_elementBits((uint16)elementBits),
    m_lanes((uint16)lanes)
{
    CHECK(elementBits <= 0xFFFF);
    CHECK((lanes >= 1) && (lanes <= 0xFFFF));
    CHECK((kind == KIND_FLOAT) == (format != FLOAT_NONE));
}

SemanticType SemanticType::integer(uint bits)
{
    CHECK(bits >= 1);
    return SemanticType(KIND_INTEGER, bits);
}

SemanticType SemanticType::floating(FloatFormat format)
{
    uint bits;
    switch (format)
    {
    case FLOAT_E4M3:
    case FLOAT_E5M2:     bits = 8;   break;
    case FLOAT_BFLOAT16:
    case FLOAT_HALF:     bits = 16;  break;
    case FLOAT_SINGLE:   bits = 32;  break;
    case FLOAT_DOUBLE:   bits = 64;  break;
    case FLOAT_EXTENDED: bits = 80;  break;
    case FLOAT_QUAD:     bits = 128; break;
    default:
        CHECK_FAIL();
    }
    return SemanticType(KIND_FLOAT, bits, 1, format);
}

SemanticType SemanticType::vector(const SemanticType& element, uint lanes)
{
    // A vector of vectors is not a type
    CHECK(!element.isVoid() && !element.isVector());
    return SemanticType((Kind)element.m_kind, element.m_elementBits, lanes,
                        (FloatFormat)element.m_floatFormat);
}

SemanticType SemanticType::mask(uint lanes)
{
    return SemanticType(KIND_MASK, 1, lanes);
}

SemanticType SemanticType::scalable(const SemanticType& type)
{
    SemanticType result(type);
    result.m_isScalable = true;
    return result;
}

SemanticType::Kind SemanticType::getKind() const
{
    return (Kind)m_kind;
}

SemanticType::FloatFormat SemanticType::getFloatFormat() const
{
    return (FloatFormat)m_floatFormat;
}

uint SemanticType::getElementBits() const
{
    return m_elementBits;
}

uint SemanticType::getLanes() const
{
    return m_lanes;
}

uint SemanticType::getTotalBits() const
{
    return (uint)m_elementBits * m_lanes;
}

SemanticType SemanticType::getElementType() const
{
    return SemanticType((Kind)m_kind, m_elementBits, 1,
                        (FloatFormat)m_floatFormat);
}

bool SemanticType::isVector() const
{
    return m_lanes > 1;
}

bool SemanticType::isVoid() const
{
    return m_kind == KIND_VOID;
}

bool SemanticType::isScalable() const
{
    return m_isScalable;
}

bool SemanticType::operator == (const SemanticType& other) const
{
    return (m_kind == other.m_kind) &&
           (m_floatFormat == other.m_floatFormat) &&
           (m_isScalable == other.m_isScalable) &&
           (m_elementBits == other.m_elementBits) &&
           (m_lanes == other.m_lanes);
}

bool SemanticType::operator != (const SemanticType& other) const
{
    return !(*this == other);
}
