#include "dismount/dismount.h"
#include "xStl/types.h"
#include "xStl/stream/ioStream.h"
#include "dismount/assembler/DependencyException.h"

static const lpString DEPENDENCY_EXCEPTION = XSTL_STRING("Dependency exception");

DependencyException::DependencyException(char * file, uint32 line,
                        const BinaryDependencies::DependencyObject& dependency,
                        uint desiredDistance) :
    cException(file, line, NULL, 0),
    m_faultDependency(dependency),
    m_desiredDistance(desiredDistance)
{
}

const BinaryDependencies::DependencyObject& DependencyException::getFaultDependency() const
{
    return m_faultDependency;
}

uint DependencyException::getDesiredDistance() const
{
    return m_desiredDistance;
}

const character* DependencyException::getMessage()
{
    return DEPENDENCY_EXCEPTION;
}

void DependencyException::print()
{
    cException::print();

    cString faultString("At dependency: ");
    faultString+= m_faultDependency.m_name;
    faultString+= endl;
    faultString+= "Location: ";
    faultString+= cString((uint)(m_faultDependency.m_position));
    faultString+= "  type: ";
    faultString+= cString((uint)(m_faultDependency.m_type));
    faultString+= endl;

    TRACE(TRACE_VERY_HIGH, faultString);
    cerr << faultString.getBuffer();
}

cException* DependencyException::clone() const
{
    return new DependencyException(*this);
}
