#include "dismount/dismount.h"
#include "dismount/assembler/SecondPassInfoAndDebug.h"

const cString SecondPassInfoAndDebug::gNoExport;

SecondPassInfoAndDebug::SecondPassInfoAndDebug()
{
}

SecondPassInfoAndDebug::SecondPassInfoAndDebug(const cString& methodName,
                                               const cString& exportName) :
     m_methodName(methodName),
     m_exportName(exportName)
{
}

const cString& SecondPassInfoAndDebug::getMethodName() const
{
    return m_methodName;
}

const cString& SecondPassInfoAndDebug::getExportName() const
{
    return m_exportName;
}

void SecondPassInfoAndDebug::setMethodName(const cString& methodName)
{
    m_methodName = methodName;
}

void SecondPassInfoAndDebug::setExportName(const cString& exportName)
{
    m_exportName = exportName;
}

bool SecondPassInfoAndDebug::isValid() const
{
    return true;
}

void SecondPassInfoAndDebug::deserialize(basicInput& inputStream)
{
    m_methodName = inputStream.readAsciiNullString();
    m_exportName = inputStream.readAsciiNullString();
}

void SecondPassInfoAndDebug::serialize(basicOutput& outputStream) const
{
    outputStream.writeAsciiNullString(m_methodName);
    outputStream.writeAsciiNullString(m_exportName);
}
