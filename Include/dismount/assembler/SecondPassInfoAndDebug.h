#ifndef __TBA_DISMOUNT_ASSEMBLER_SECONDPASSINFOANDDEBUG_H
#define __TBA_DISMOUNT_ASSEMBLER_SECONDPASSINFOANDDEBUG_H

/*
 * SecondPassInfoAndDebug.h
 *
 * Save all the method information and debug information
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/data/array.h"
#include "xStl/data/string.h"
#include "xStl/data/smartptr.h"
#include "xStl/data/serializedObject.h"

/*
 * After the method upon it's basic blocks, was compiled, this class reassemble
 * the method internally into fixed size block
 */
class SecondPassInfoAndDebug : public cSerializedObject {
public:
    // Define the function as non-export
    static const cString gNoExport;

    /*
     * Constructor.
     *
     * methodName - The name of the function (as is)
     * exportName - Define the method as exported
     */
    SecondPassInfoAndDebug(const cString& methodName,
                           const cString& exportName);
    SecondPassInfoAndDebug();

    /*
     * Get function, return the name of the method/export
     */
    const cString& getMethodName() const;
    const cString& getExportName() const;

    /*
     * Change the name of the method/export
     */
    void setMethodName(const cString& methodName);
    void setExportName(const cString& exportName);

    // See cSerializedObject::isValid
    virtual bool isValid() const;
    // See cSerializedObject::deserialize
    virtual void deserialize(basicInput& inputStream);
    // See cSerializedObject::serialize
    virtual void serialize(basicOutput& outputStream) const;

private:
    cString m_methodName;
    cString m_exportName;
};


#endif // __TBA_DISMOUNT_ASSEMBLER_SECONDPASSINFOANDDEBUG_H
