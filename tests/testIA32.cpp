/*
 * testIA32.cpp
 *
 * Tests the
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/os/os.h"
#include "xStl/os/threadUnsafeMemoryAccesser.h"
#include "xStl/data/datastream.h"
#include "xStl/utils/algorithm.h"
#include "xStl/except/trace.h"
#include "xStl/except/assert.h"
#include "xStl/stream/ioStream.h"
#include "xStl/stream/memoryStream.h"
#include "xStl/stream/memoryAccesserStream.h"
#include "xStl/../../tests/tests.h"
#include "dismount/Opcode.h"
#include "dismount/OpcodeSubsystems.h"
#include "dismount/StreamDisassembler.h"
#include "dismount/StreamDisassemblerFactory.h"
#include "dismount/DefaultOpcodeDataFormatter.h"
#include "dismount/proc/ia32/IA32Opcode.h"


class TestObjectTestIA32 : public cTestObject {
public:
    // The number of opcode byte to view
    enum { NUMBER_OF_OPCODE = 7,
           OPCODE_MARGIN = 10 };

    /*
     * This function tests the dismount by trying to disassemble the function
     * its self
     */
    void appTest32()
    {
        cVirtualMemoryAccesserPtr context(new cThreadUnsafeMemoryAccesser());
        addressNumericValue start = getNumeric(&CoCreateInstance);
        // start = 0x400000; // 0x0419E80;
        addressNumericValue end = start + 0x100;
        cMemoryAccesserStream stream(context, start, end);

        // Generate the disassembler
        StreamDisassemblerPtr disassembler = StreamDisassemblerFactory::disassemble(
                OpcodeSubsystems::DISASSEMBLER_INTEL_32,
                BasicInputPtr(&stream, SMARTPTR_DESTRUCT_NONE),
                true,
                ProcessorAddress(ProcessorAddress::PROCESSOR_32, start));

        XSTL_TRY
        {
            // Prepare the default data-formatter
            DefaultOpcodeDataFormatter formatter(OPCODE_MARGIN);

            while (true)
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

                // Show address, if avaliable
                ProcessorAddress address(gNullPointerProcessorAddress);
                if (opcode->getOpcodeAddress(address))
                    cout << formatter.translateAbsoluteAddress(address) << "   ";

                // Show opcode bytes
                cout << opcodeString;

                // Show the complete formatted opcode
                cout << fr->string() << endl;
            }
        } XSTL_CATCH_ALL
        {
            // End-of-stream, or error, let's check it

        }
    }

    void test32()
    {
        #ifndef XSTL_64BIT
            appTest32();
        #endif
    }

    virtual void test()
    {
        // Test the 32bit opcode table
        test32();
        // Test the 16bit opcode table
        // test16();
    }

    // Return the name of the module
    virtual cString getName() { return __FILE__; }
};

// Instance test object
TestObjectTestIA32 g_globalTestIA32;
