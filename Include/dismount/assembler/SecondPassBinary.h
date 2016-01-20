#ifndef __TBA_DISMOUNT_ASSEMBLER_SECONDPASSBINARY_H
#define __TBA_DISMOUNT_ASSEMBLER_SECONDPASSBINARY_H

/*
 * SecondPassBinary.h
 *
 * After the first pass binary was finished in creating all basic blocks, it can
 * generate a second-pass binary which hold a method ready for execution, except
 * for other methods relocations.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/data/array.h"
#include "xStl/data/smartptr.h"
#include "xStl/data/serializedObject.h"
#include "xStl/stream/stringerStream.h"
#include "dismount/assembler/BinaryDependencies.h"
#include "dismount/assembler/FirstPassBinary.h"
#include "dismount/assembler/SecondPassInfoAndDebug.h"

// Trace routines
#ifdef _DEBUG
class SecondPassBinary;
cStringerStream& operator << (cStringerStream& out,
                              const SecondPassBinary& data);
#endif

/*
 * After the method upon it's basic blocks, was compiled, this class reassemble
 * the method internally into fixed size block
 */
class SecondPassBinary : public cSerializedObject {
public:
    /*
     * Special enum for block alignment. See SecondPassBinary constructor.
     */
     enum BlocksType {
         NO_BLOCKS_ALIGN = 0xFFFFFFFC,
         ALL_BLOCKS_ALIGN = 0xFFFFFFFE
     };

    // This struct defines a possible resolver for dependencies
    struct ResolveHelper
    {
    public:
        // Ctor
        ResolveHelper(SecondPassBinary* pResolver, const cString& sResolverName, FirstPassBinaryPtr firstPass);
        ResolveHelper(const ResolveHelper& other);

    public:
        // Members
        SecondPassBinary* m_pResolver;
        cString m_sResolverName;
        FirstPassBinaryPtr m_firstPass;
    };

    // This type defines a list of resolvers for dependencies
    typedef cList<ResolveHelper> ResolveHelperList;

    /*
     * Constructor. From a first binary pass, returns a second binary pass
     *
     * blockNumber - Select a block to add alignment to. NO_BLOCKS_ALIGN - don't align any blocks. ALL_BLOCKS_ALIGN - Align every basic block
     * alignTo     - Number of bytes to align to
     * alignBuffer - The pattern of the alignment. NULL means 0's. NOTE: The buffer length must be at alignment size
     */
    SecondPassBinary(FirstPassBinary& firstPass,
                    uint blockNumber,
                    uint alignTo = sizeof(uint32),
                    cBufferPtr alignBuffer = cBufferPtr(),
                    ResolveHelperList* pResolveHelpers = NULL);

    /*
     * Constructor from stream
     */
    SecondPassBinary(basicInput& inputStream);

    /*
     * Get/set debug information
     */
    SecondPassInfoAndDebug& getDebugInformation();
    const SecondPassInfoAndDebug& getDebugInformation() const;

    /*
     * Return the opcode of the binary data
     */
    const cBuffer& getData() const;

    /*
     * Return the dependencies list
     */
    const BinaryDependencies& getDependencies() const;

    /*
     * Return the assembly type
     */
    OpcodeSubsystems::DisassemblerType getAssemblerType() const;

    void resolveStack(BinaryDependencies::DependencyObject& dependency,
                      FirstPassBinary& firstPass,
                      addressNumericValue binaryAddress,
                      addressNumericValue dependencyAddress,
                      uint dataStartAddress = 0);

    /*
     * Replace a dependency
     *
     * dependency        - The dependency to replace
     * binaryAddress     - The address of the current block
     * dependencyAddress - The address of the dependency
     *
     * dataStartAddress  - [For inside use only] The inside address
     */
    void resolveDependency(BinaryDependencies::DependencyObject& dependency,
                           addressNumericValue binaryAddress,
                           addressNumericValue dependencyAddress,
                           uint dataStartAddress = 0,
                           bool shouldZeroData = false);

    // See cSerializedObject::isValid
    virtual bool isValid() const;
    // See cSerializedObject::deserialize
    virtual void deserialize(basicInput& inputStream);
    // See cSerializedObject::serialize
    virtual void serialize(basicOutput& outputStream) const;

private:
    /*
     * Expand the sign bit of a number
     */
    static int makeInt(uint number, uint numberSizeInBits);

    // The number of bytes for best method allocation
    static const uint gMethodPage;

    // Each block and it's global position.
    cHash<int, uint> m_blocksPositions;

    // The data itself
    cBuffer m_data;
    // The combine dependency
    BinaryDependencies m_dependency;
    // The type of the assembly
    OpcodeSubsystems::DisassemblerType m_type;
    // The debug information
    SecondPassInfoAndDebug m_debug;
};

// The reference countable object
typedef cSmartPtr<SecondPassBinary> SecondPassBinaryPtr;

#endif // __TBA_DISMOUNT_ASSEMBLER_SECONDPASSBINARY_H
