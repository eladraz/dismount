#include "dismount/dismount.h"
/*
 * SecondPassBinary.cpp
 *
 * Implementation file
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/except/trace.h"
#include "xStl/except/assert.h"
#include "xStl/data/alignment.h"
#include "xStl/data/array.h"
#include "xStl/data/list.h"
#include "xStl/data/hash.h"
#include "xStl/data/datastream.h"
#include "xStl/os/os.h"
#include "xStl/stream/memoryStream.h"
#include "xStl/stream/stringerStream.h"
#include "dismount/assembler/DependencyException.h"
#include "dismount/assembler/MangledNames.h"
#include "dismount/assembler/SecondPassBinary.h"
// Trace routine includes
#ifdef _DEBUG
#include "dismount/StreamDisassemblerFactory.h"
#include "dismount/DefaultOpcodeDataFormatter.h"
#include "dismount/Opcode.h"
#endif

#define CHECK_RELOCATION_RANGE(distance, max_relocation) { if (distance >= (max_relocation))  XSTL_THROW(DependencyException, dependency, distance); }

const uint SecondPassBinary::gMethodPage = 1024;

SecondPassBinary::SecondPassBinary(FirstPassBinary& firstPass, uint blockNumber, uint alignTo, cBufferPtr alignBuffer, ResolveHelperList* pResolveHelpers) :
    m_data((uint)(0), gMethodPage)
{
    m_type = firstPass.getAssemblerType();
    // Combine all basic blocks
    const FirstPassBinary::BasicBlockList& blocks = firstPass.getBlocksList();

    // And combine blocks
    FirstPassBinary::BasicBlockList::iterator i = blocks.begin();
    uint j = 0;
    for (; i != blocks.end(); ++i, ++j)
    {
        // Adding block information for specific arch
        if (firstPass.getAssemblerType() == OpcodeSubsystems::DISASSEMBLER_STRING)
        {
            if ((*i).m_blockNumber >= 0)
            {
                cString block(endl);
                block+= "cblk";
                block+= cString((*i).m_blockNumber);
                block+= ":";
                block+= endl;
                cSArray<char> a = block.getASCIIstring();
                cBuffer newBuffer((const uint8*)a.getBuffer(), a.getSize() - 1);
                m_data.append(newBuffer);
            }
        }

        m_blocksPositions.append((*i).m_blockNumber, m_data.getSize());
        m_data+= (*i).m_data;
        // DismountTrace("SecondPassBinary block: " << HEXDWORD((*i).m_blockNumber) << " " << "size: " << HEXDWORD((*i).m_data.getSize()) << " "
        //              << "soffset: " << HEXDWORD(m_data.getSize() - (*i).m_data.getSize()) << " " << "eoffset: " << HEXDWORD(m_data.getSize()) << endl);

        if ((blockNumber == (uint)SecondPassBinary::ALL_BLOCKS_ALIGN) ||
            ((blockNumber != (uint)SecondPassBinary::NO_BLOCKS_ALIGN) && (uint)((*i).m_blockNumber) == blockNumber))
        {
            uint oldSize = m_data.getSize();
            uint newSize = Alignment::alignUp(oldSize, alignTo-1);
            if (newSize > oldSize)
            {
                m_data.changeSize(newSize);
                if (alignBuffer.isEmpty())
                {
                    memset(m_data.getBuffer() + oldSize, 0, newSize - oldSize);
                } else
                {
                    CHECK((newSize - oldSize) <= alignBuffer->getSize());
                    cOS::memcpy(m_data.getBuffer() + oldSize, alignBuffer->getBuffer(), newSize - oldSize);
                }
            }
        }
    }

    // Relocate all sections
    i = blocks.begin();
    j = 0;
    for (; i != blocks.end(); ++i)
    {
        uint pos = m_blocksPositions[(*i).m_blockNumber];
        const BinaryDependencies& dependencies = (*i).m_dependecies;
        const BinaryDependencies::DependencyObjectList& depList =
                                                         dependencies.getList();

        // Insert label for basic block
        cString name("Basic block ");
        name+= HEXDWORD((*i).m_blockNumber);
        m_dependency.addDependency(name,
                                   pos,
                                   (*i).m_data.getSize(),
                                   BinaryDependencies::DEP_LABEL);


        BinaryDependencies::DependencyObjectList::iterator x = depList.begin();
        for (; x != depList.end(); ++x)
        {
            // Check whether the relocation name is for another basic block
            if (MangledNames::isBasicBlockSymbol((*x).m_name))
            {
                // Read mangling. And check dependency
                int blockID;
                uint length;
                BinaryDependencies::DependencyType type;
                MangledNames::readBasicBlockSymbol((*x).m_name, blockID,
                    length, type);
                // Check symbol compatibility
                ASSERT(type == (*x).m_type);
                ASSERT(length == length);

                // Replace the relocation
                switch((*x).m_type)
                {
                case BinaryDependencies::DEP_STACK_ARG:
                case BinaryDependencies::DEP_STACK_LOCAL:
                case BinaryDependencies::DEP_STACK_TEMP:
                    {
                        // Resolve stack
                        if ((*x).m_bUseParentFirstPass)
                        {
                            // This dependency is to the parent firstpass, not to this firstpass
                            ASSERT((pResolveHelpers != NULL) && !pResolveHelpers->isEmpty());
                            ResolveHelperList::iterator iResolver = pResolveHelpers->end() - 1;
                            resolveStack(*x, *(*iResolver).m_firstPass, pos, m_blocksPositions[blockID], pos);
                        }
                        else
                            resolveStack(*x, firstPass, pos, m_blocksPositions[blockID], pos);
                    }
                    break;
                case BinaryDependencies::DEP_RELATIVE:
                    {
                        // And resolve
                        addressNumericValue depAddress;

                        // Try to resolve using this binary
                        if (m_blocksPositions.hasKey(blockID))
                        {
                            depAddress = m_blocksPositions[blockID];
                            resolveDependency(*x, pos, depAddress, pos);
                            break;
                        }

                        // This binary can't resolve. Try to resolve using the provided helpers
                        if (pResolveHelpers != NULL)
                        {
                            ResolveHelperList::iterator iResolver = pResolveHelpers->begin();
                            for (; iResolver != pResolveHelpers->end(); iResolver++)
                            {
                                if ((*iResolver).m_pResolver->m_blocksPositions.hasKey(blockID))
                                    break;
                            }
                            if (iResolver == pResolveHelpers->end())
                            {
                                // No resolver could resolve this dependency! This is a bug.
                                CHECK(FALSE);
                            }
                            // Calculate target address within the other function
                            depAddress = (*iResolver).m_pResolver->m_blocksPositions[blockID];
                            // Append a dependency to that method, at that place
                            m_dependency.addDependency((*iResolver).m_sResolverName,
                                (*x).m_position + pos,
                                (*x).m_length,
                                (*x).m_type,
                                (*x).m_shiftRightCount,
                                (*x).m_shouldAddExistValue,
                                (*x).m_fixOffset + depAddress);
                        }
                    }
                    break;
                default:
                    // TODO!
                    CHECK_FAIL();
                }
            } else
            {
                // Just append symbol with code shifting
                m_dependency.addDependency((*x).m_name,
                                           (*x).m_position + pos,
                                           (*x).m_length,
                                           (*x).m_type,
                                           (*x).m_shiftRightCount,
                                           (*x).m_shouldAddExistValue,
                                           (*x).m_fixOffset);
            }
        }
    }
}

SecondPassBinary::SecondPassBinary(basicInput& inputStream)
{
    deserialize(inputStream);
}

SecondPassInfoAndDebug& SecondPassBinary::getDebugInformation()
{
    return m_debug;
}


const SecondPassInfoAndDebug& SecondPassBinary::getDebugInformation() const
{
    return m_debug;
}

const cBuffer& SecondPassBinary::getData() const
{
    return m_data;
}

const BinaryDependencies& SecondPassBinary::getDependencies() const
{
    return m_dependency;
}

OpcodeSubsystems::DisassemblerType SecondPassBinary::getAssemblerType() const
{
    return m_type;
}

void SecondPassBinary::resolveStack(BinaryDependencies::DependencyObject& dependency, FirstPassBinary& firstPass, addressNumericValue binaryAddress, addressNumericValue dependencyAddress, uint dataStartAddress)
{
    uint relocatePositionTo = dependencyAddress;
    uint currentPosition = binaryAddress + dependency.m_position;
    int distance = relocatePositionTo;
    distance-= currentPosition;

    switch (dependency.m_type)
    {
    case BinaryDependencies::DEP_STACK_ARG:
        distance = firstPass.getStackSize() + firstPass.getNonVolatilesSize();
        break;
    case BinaryDependencies::DEP_STACK_LOCAL:
        distance = firstPass.getStackSize();
        break;
    case BinaryDependencies::DEP_STACK_TEMP:
        distance = firstPass.getStackSize() - firstPass.getStackBaseSize();
        break;
    default:
        CHECK_FAIL();
    }

    switch (dependency.m_length)
    {
    case BinaryDependencies::DEP_8BIT:
        {
            if (dependency.m_shouldAddExistValue)
                distance += ((char)m_data[dataStartAddress + dependency.m_position]);

            // If we fails here, it means that the basic-block should have been arranged better
            CHECK_RELOCATION_RANGE(t_abs(distance), 0x100);

            distance >>= dependency.m_shiftRightCount;
            m_data[dataStartAddress + dependency.m_position] = ((int8)(distance));
        }
        break;

    /*
    case BinaryDependencies::DEP_3BIT_3SKIP:
        {
            uint16 oldValue = cEndian::readUint16(m_data.getBuffer() + dependency.m_position + dataStartAddress,
                OpcodeSubsystems::isLittleEndian(m_type));
            if (dependency.m_shouldAddExistValue)
            {
                // TODO! Pavel - It's doesn't look right...
                distance += makeInt(((oldValue >> 3) & 0x07), 3);
            }

            // If we fails here, it means that the basic-block should have been arranged better
            CHECK_RELOCATION_RANGE(t_abs(distance), 0x100);

            // TODO! Pavel - It's doesn't look right...
            oldValue = (oldValue & 0xF83F) + ((distance & 3) << 6) + ((distance & 0x1C) << 4);

            cEndian::writeUint16(m_data.getBuffer() + dependency.m_position + dataStartAddress,
                oldValue,
                OpcodeSubsystems::isLittleEndian(m_type));
        }
        break;
    */

    case BinaryDependencies::DEP_4BIT_LOWER:
        {
            char oldVal = ((char)m_data[dataStartAddress + dependency.m_position]);
            if (dependency.m_shouldAddExistValue)
                distance += makeInt(oldVal & 0x0F, 4);

            distance >>= dependency.m_shiftRightCount;
            CHECK_RELOCATION_RANGE(t_abs(distance), 0x10);

            m_data[dataStartAddress + dependency.m_position] = ((int8)((oldVal & 0xF0) + (distance & 0x0F)));
        }
        break;

    case BinaryDependencies::DEP_5BIT_5SPACE:
        {
            uint16 oldValue = cEndian::readUint16(m_data.getBuffer() + dependency.m_position + dataStartAddress,
                OpcodeSubsystems::isLittleEndian(m_type));
            if (dependency.m_shouldAddExistValue)
            {
                distance+= makeInt((((oldValue >> 8) << 2) + (oldValue & 3)) & 0x1F, 5);
            }

            oldValue = (oldValue & 0xF83F) + ((distance & 3) << 6) + ((distance & 0x1C) << 4);
            CHECK_RELOCATION_RANGE(t_abs(distance), 0x20);

            cEndian::writeUint16(m_data.getBuffer() + dependency.m_position + dataStartAddress,
                oldValue,
                OpcodeSubsystems::isLittleEndian(m_type));
        }
        break;

    case BinaryDependencies::DEP_8BIT_4SPACE:
        {
            uint16 oldValue = cEndian::readUint16(m_data.getBuffer() + dependency.m_position + dataStartAddress,
                OpcodeSubsystems::isLittleEndian(m_type));
            if (dependency.m_shouldAddExistValue)
            {
                distance += makeInt(((oldValue & 0xF00) >> 4) + (oldValue & 0x0F), 8);
            }

            CHECK_RELOCATION_RANGE(t_abs(distance), 0x100);
            oldValue = ((distance & 0xF0) << 4) + (oldValue & 0xF0F0) + (distance & 0x0F);

            cEndian::writeUint16(m_data.getBuffer() + dependency.m_position + dataStartAddress,
                oldValue,
                OpcodeSubsystems::isLittleEndian(m_type));
        }
        break;

    case BinaryDependencies::DEP_12BIT:
        {
            uint16 oldValue = cEndian::readUint16(m_data.getBuffer() + dependency.m_position + dataStartAddress,
                OpcodeSubsystems::isLittleEndian(m_type));

            if (dependency.m_shouldAddExistValue)
            {
                distance += makeInt(oldValue & 0xFFF, 12);
            }

            // If we fails here, it means that the basic-block should have been arranged better
            CHECK_RELOCATION_RANGE(t_abs(distance), 0x1000);

            oldValue = (oldValue & ~0xFFF) | (distance & 0xFFF);

            cEndian::writeUint16(m_data.getBuffer() + dependency.m_position + dataStartAddress,
                oldValue,
                OpcodeSubsystems::isLittleEndian(m_type));
        }
        break;
    default:
        CHECK_FAIL();
    }
}

void SecondPassBinary::resolveDependency(BinaryDependencies::DependencyObject& dependency,
                            addressNumericValue binaryAddress,
                            addressNumericValue dependencyAddress,
                            uint dataStartAddress,
                            bool shouldZeroData)
{
    if ((dependency.m_type) == BinaryDependencies::DEP_RELATIVE)
    {
        // Relocate
        uint relocatePositionTo = dependencyAddress;
        uint currentPosition = binaryAddress + dependency.m_position;
        int distance = relocatePositionTo;
        distance-= currentPosition;
        switch (dependency.m_length)
        {
        case BinaryDependencies::DEP_8BIT:
            if (dependency.m_shouldAddExistValue)
                distance += ((char)m_data[dataStartAddress + dependency.m_position]);

            if (shouldZeroData)
                distance = (int8)dependencyAddress;
            else
                distance >>= dependency.m_shiftRightCount;

            // Making sure we are not overflowing
            CHECK_RELOCATION_RANGE(t_abs(distance), 0x100);
            // CHECK(t_abs(distance) < (1 << (8 + dependency.m_shiftRightCount - 1)));

            m_data[dataStartAddress + dependency.m_position] = ((int8)(distance));
            break;

        case BinaryDependencies::DEP_11BIT:
            {
                uint16 oldValue = cEndian::readUint16(m_data.getBuffer() + dataStartAddress + dependency.m_position,
                                                      OpcodeSubsystems::isLittleEndian(m_type));

                distance >>= dependency.m_shiftRightCount;

                if (dependency.m_shouldAddExistValue)
                {
                    distance += makeInt(oldValue & 0x7FF, 11);
                }

                if (shouldZeroData)
                    distance = dependencyAddress;

                // Making sure we are not overflowing
                CHECK_RELOCATION_RANGE(t_abs(distance), 0x800);
                // CHECK(t_abs(distance) < (1 << (11 + dependency.m_shiftRightCount - 1)));

                oldValue = (oldValue & ~0x7FF) | (distance & 0x7FF);

                cEndian::writeUint16(m_data.getBuffer() + dataStartAddress + dependency.m_position,
                                     oldValue,
                                     OpcodeSubsystems::isLittleEndian(m_type));
            }
            break;

        case BinaryDependencies::DEP_12BIT:
            {
                uint16 oldValue = cEndian::readUint16(m_data.getBuffer() + dataStartAddress + dependency.m_position,
                    OpcodeSubsystems::isLittleEndian(m_type));

                distance >>= dependency.m_shiftRightCount;

                if (dependency.m_shouldAddExistValue)
                {
                    distance += makeInt(oldValue & 0xFFF, 12);
                }

                if (shouldZeroData)
                    distance = dependencyAddress;

                // Making sure we are not overflowing
                CHECK_RELOCATION_RANGE(t_abs(distance), 0x1000);
                // CHECK(t_abs(distance) < (1 << (12 + dependency.m_shiftRightCount - 1)));

                oldValue = (oldValue & ~0xFFF) | (distance & 0xFFF);

                cEndian::writeUint16(m_data.getBuffer() + dataStartAddress + dependency.m_position,
                    oldValue,
                    OpcodeSubsystems::isLittleEndian(m_type));
            }
            break;

        case BinaryDependencies::DEP_19BIT_2BYTES_LITTLE_ENDIAN:
            {
                uint32 oldValue = cEndian::readUint32(m_data.getBuffer() + dataStartAddress + dependency.m_position,
                                                      OpcodeSubsystems::isLittleEndian(m_type));
                distance >>= dependency.m_shiftRightCount;

                if (dependency.m_shouldAddExistValue)
                {
                    // Taking bits 1:11 + 16:21 + 12 + 14
                    uint32 oldAddress = (((oldValue >> 16) & 0x7FF)) |
                                        (((oldValue >>  0) & 0x3F) << 11) |
                                        (((oldValue >> 27) & 1) << 17) |
                                        (((oldValue >> 29) & 1) << 18);
                    distance += makeInt(oldAddress, 19);
                }

                if (shouldZeroData)
                    distance = dependencyAddress;

                // Making sure we are not overflowing
                CHECK_RELOCATION_RANGE(t_abs(distance), 0x80000);
                //CHECK(t_abs(distance) < (1 << (19 + dependency.m_shiftRightCount - 1)));

                oldValue = (oldValue & ~0x2FFF003F) | // Clearing out old value
                           (((distance >> 0) & 0x7FF) << 16) |
                           (((distance >> 11) & 0x3F) <<  0) |
                           (((distance >> 17) &  0x1) << 27) |
                           (((distance >> 18) &  0x1) << 29);

                cEndian::writeUint32(m_data.getBuffer() + dataStartAddress + dependency.m_position,
                                     oldValue,
                                     OpcodeSubsystems::isLittleEndian(m_type));
            }
            break;

        case BinaryDependencies::DEP_22BIT_2BYTES_LITTLE_ENDIAN:
            {
                uint32 oldValue = cEndian::readUint32(m_data.getBuffer() + dataStartAddress + dependency.m_position,
                                                      OpcodeSubsystems::isLittleEndian(m_type));
                distance >>= dependency.m_shiftRightCount;

                if (dependency.m_shouldAddExistValue)
                {
                    distance += makeInt(((oldValue & 0x7FF) << 11) | ((oldValue >> 16) & 0x7FF), 22);
                }

                if (shouldZeroData)
                    distance = dependencyAddress;

                // Making sure we are not overflowing
                CHECK_RELOCATION_RANGE(t_abs(distance), 0x400000);
                //CHECK(t_abs(distance) < (1 << (22 + dependency.m_shiftRightCount - 1)));

                oldValue = (oldValue & ~0x07FF07FF) | (((uint32)distance & 0x7FF)  << 16) | (((uint32)distance >> 11) & 0x7FF);

                cEndian::writeUint32(m_data.getBuffer() + dataStartAddress + dependency.m_position,
                                     oldValue,
                                     OpcodeSubsystems::isLittleEndian(m_type));
            }
            break;

        case BinaryDependencies::DEP_24BIT:
            {
                uint32 oldValue = cEndian::readUint32(m_data.getBuffer() + dataStartAddress + dependency.m_position,
                    OpcodeSubsystems::isLittleEndian(m_type));

                distance >>= dependency.m_shiftRightCount;
                if (dependency.m_shouldAddExistValue)
                {
                    distance += makeInt((oldValue & 0xFFFFFF), 24);
                }

                if (shouldZeroData)
                    distance = dependencyAddress;

                // Making sure we are not overflowing
                CHECK_RELOCATION_RANGE(t_abs(distance), 0x1000000);

                oldValue = (oldValue & 0xFF000000) + (distance & 0xFFFFFF);

                cEndian::writeUint32(m_data.getBuffer() + dataStartAddress + dependency.m_position,
                                     oldValue,
                                     OpcodeSubsystems::isLittleEndian(m_type));
            }
            break;

        case BinaryDependencies::DEP_32BIT:
            {
                uint32 oldValue = cEndian::readUint32(m_data.getBuffer() + dataStartAddress + dependency.m_position,
                    OpcodeSubsystems::isLittleEndian(m_type));

                distance >>= dependency.m_shiftRightCount;
                if (dependency.m_shouldAddExistValue)
                {
                    distance += (int32)oldValue;
                }
                // Fix offset is used to adjust the distance in case of opcodes which calculate the distance, such as: jmp, call.
                distance += dependency.m_fixOffset;
                oldValue = distance & 0xFFFFFFFF;

                if (shouldZeroData)
                    oldValue = dependencyAddress;

                cEndian::writeUint32(m_data.getBuffer() + dataStartAddress + dependency.m_position,
                                     oldValue,
                                     OpcodeSubsystems::isLittleEndian(m_type));
            }
            break;

        default:
            // TODO! Add 2,8 bytes dependency resolvers
            CHECK_FAIL();
        }
    } else if ((dependency.m_type) == BinaryDependencies::DEP_ABSOLUTE)
    {
        switch (dependency.m_length)
        {
        case 1:
            // 8 bit relocating
            CHECK_RELOCATION_RANGE(dependencyAddress, 0x80);
            //CHECK(dependencyAddress < 0x80);
            if (OpcodeSubsystems::isOverwriteRelativeLocations(m_type))
                dependencyAddress = dependencyAddress + m_data[dataStartAddress + dependency.m_position];

            m_data[dataStartAddress + dependency.m_position] = ((int8)(dependencyAddress));
            break;

        case 4:
            // Encoding little/big-big endian
            if (OpcodeSubsystems::isLittleEndian(m_type))
            {
                if (!shouldZeroData && OpcodeSubsystems::isOverwriteRelativeLocations(m_type))
                {
                    dependencyAddress = dependencyAddress + cLittleEndian::readUint32(
                        m_data.getBuffer() +
                        dataStartAddress +
                        dependency.m_position);
                }


                cLittleEndian::writeUint32(
                    m_data.getBuffer() + dataStartAddress + dependency.m_position,
                    dependencyAddress);
            }
            else
            {
                if (!shouldZeroData && OpcodeSubsystems::isOverwriteRelativeLocations(m_type))
                {
                    dependencyAddress = dependencyAddress + cBigEndian::readUint32(
                        m_data.getBuffer() +
                        dataStartAddress +
                        dependency.m_position);
                }

                cBigEndian::writeUint32(
                    m_data.getBuffer() + dataStartAddress + dependency.m_position,
                    dependencyAddress);
            }

            break;
        default:
            // TODO! Add 2,8 bytes dependency resolvers
            CHECK_FAIL();
        }
    } else
    {
        // TODO!
        CHECK_FAIL();
    }
}

int SecondPassBinary::makeInt(uint number, uint numberSizeInBits)
{
    int ret = (int)number;
    int signned = (number >> (numberSizeInBits - 1)) & 1;
    ret|= ~((signned << numberSizeInBits) - 1);
    return ret;
}

bool SecondPassBinary::isValid() const
{
    return true;
}

void SecondPassBinary::deserialize(basicInput& inputStream)
{
    uint32 type;
    inputStream.streamReadUint32(type);
    m_type = (OpcodeSubsystems::DisassemblerType)type;
    m_debug.deserialize(inputStream);
    m_dependency.deserialize(inputStream);
    m_data.deserialize(inputStream);
}

void SecondPassBinary::serialize(basicOutput& outputStream) const
{
    outputStream.streamWriteUint32(m_type);
    m_debug.serialize(outputStream);
    m_dependency.serialize(outputStream);
    m_data.serialize(outputStream);
}

SecondPassBinary::ResolveHelper::ResolveHelper(SecondPassBinary* pResolver, const cString& sResolverName, FirstPassBinaryPtr firstPass)
    : m_pResolver(pResolver),
      m_sResolverName(sResolverName),
      m_firstPass(firstPass)
{
}

SecondPassBinary::ResolveHelper::ResolveHelper(const ResolveHelper& other) :
    m_pResolver(other.m_pResolver),
    m_sResolverName(other.m_sResolverName),
    m_firstPass(other.m_firstPass)
{
}

//////////////////////////////////////////////////////////////////////////
// Trace routine
#ifdef _DEBUG

// The number of opcode byte to view
enum { NUMBER_OF_OPCODE = 7,
       OPCODE_MARGIN = 10 };

cStringerStream& operator << (cStringerStream& out,
                              const SecondPassBinary& data)
{
    // Generate the disassembler
    cMemoryStream stream(data.getData());
    StreamDisassemblerPtr disassembler = StreamDisassemblerFactory::disassemble(
                    data.getAssemblerType(),
                    BasicInputPtr(&stream, SMARTPTR_DESTRUCT_NONE),
                    true,
                    // TODO!
                    ProcessorAddress(ProcessorAddress::PROCESSOR_32, 0));

    // Prepare the default data-formatter
    DefaultOpcodeDataFormatter formatter(OPCODE_MARGIN);

    XSTL_TRY
    {
        while ((stream.length() - stream.getPointer()) > 0)
        {
            OpcodePtr opcode = disassembler->next();
            cBuffer opcodeData = opcode->getOpcode();

            // Format the opcode-data-bytes
            cString opcodeString;
            int i = 0;
            for (; i < t_min((int)NUMBER_OF_OPCODE, (int)opcodeData.getSize()); i++)
            {
                opcodeString+= HEXBYTE(opcodeData[i]) + " ";
            }
            for (i = 0; i < NUMBER_OF_OPCODE - (int)opcodeData.getSize(); i++) {
                opcodeString+= "   ";
            }
            opcodeString+= "  ";

            // Get the translated opcode
            OpcodeFormatterPtr fr = disassembler->getOpcodeFormat(opcode,
                formatter);

            // Flush the information to the screen

            // Show address, if available
            ProcessorAddress address(gNullPointerProcessorAddress);
            if (opcode->getOpcodeAddress(address))
            {
                out << formatter.translateAbsoluteAddress(address)
                    << "   ";
            }

            // Show opcode bytes
            out << opcodeString;
            // Show the complete formatted opcode
            out << fr->string() << endl;
        }
    } XSTL_CATCH_ALL
    {
        out << "UNKNOWN ERROR DURING DISASSEMBLER!" << endl;
    }

    return out;
}

#endif // _DEBUG
