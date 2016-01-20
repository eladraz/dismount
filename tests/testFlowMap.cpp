/*
 * testFlowMap.cpp
 *
 * Tests the flow mapping capabilities of Dismount
 *
 * Author: Tal Harel
 */
#include "xStl/types.h"
#include "xStl/data/char.h"
#include "xStl/data/string.h"
#include "xStl/os/os.h"
#include "xStl/os/threadUnsafeMemoryAccesser.h"
#include "xStl/data/datastream.h"
#include "xStl/utils/algorithm.h"
#include "xStl/except/trace.h"
#include "xStl/except/assert.h"
#include "xStl/stream/traceStream.h"
#include "xStl/stream/fileStream.h"
#include "xStl/stream/ioStream.h"
#include "xStl/stream/basicIO.h"
#include "xStl/stream/memoryAccesserStream.h"
#include "xStl/../../tests/tests.h"
#include "pe/peFile.h"
#include "pe/dosheader.h"
#include "pe/ntheader.h"
#include "pe/ntDirExport.h"
#include "pe/ntDirReloc.h"
#include "dismount/FlowMapper.h"
#include "xStl/enc/digest/md5.h"

#include "dismount/Opcode.h"
#include "dismount/InvalidOpcodeByte.h"

#ifdef XSTL_WINDOWS
    #include "dia2.h"
    #include <atlbase.h>
#endif

#define LIST_FILE_PATH  (XSTL_STRING("filelist_ntkrnlpa.txt"))
#define FILE_PATH       (XSTL_STRING("ntkrnlpa.exe"))
#define OUTPUT_FILE     (XSTL_STRING("maplist_dump.bin"))
#define PDB_FILENAME    (XSTL_STRING("ntkrnlpa.pdb"))

class TestObjectTestFlowMap : public cTestObject {
public:
    bool mapExports(FlowMapper& currMapper, const cNtDirExport::ExportTable& exportArray)
    {
        uint32 subsetsFound = 0;
        uint32 prevSubsets = 0;

        // Sort the functions by address
        boubbleSort(exportArray.begin(), exportArray.end());
        //cout << "[*] Exports to map: " << exportArray.getSize() << endl;

        XSTL_TRY
        {
            for (uint i = 0; i < exportArray.getSize(); i++)
            {
                addressNumericValue address = exportArray[i].m_address.getAddressValue();
                addressNumericValue nextAddress = 0;

                // Check if we have the next function address
                if (i < exportArray.getSize() - 1)
                {
                    nextAddress = exportArray[i + 1].m_address.getAddressValue();

                    // Ignore duplicates
                    if (address == nextAddress) continue;
                }

                //cout << "[*] Mapping " << HEXDWORD(address) << " -> " << HEXDWORD(nextAddress) << endl;
                subsetsFound = currMapper.map(address, nextAddress);
                prevSubsets = subsetsFound;
            }
        }
        XSTL_CATCH (cException& e)
        {
            cout << endl << "[!] Exception: " << e.getMessage() << " (" << e.getID() << ')' << endl;
            cout << "[!] Last opcode: " << HEXDWORD(currMapper.getLastOpcode().getAddress()) << endl;
            return false;
        }

        return true;
    }

    bool mapOffsets(FlowMapper& currMapper, cList<ProcessorAddress>& listOffsets)
    {
        uint subsetsFound = 0;
        uint prevSubsets = 0;

        // Sort the offsets by address
        boubbleSort(listOffsets.begin(), listOffsets.end());
        //cout << "[*] Offsets to map: " << listOffsets.length() << endl;

        XSTL_TRY
        {
            uint i = 0;
            for (cList<ProcessorAddress>::iterator offsetIter = listOffsets.begin(); offsetIter != listOffsets.end(); offsetIter++, i++)
            {
                addressNumericValue address = (*offsetIter).getAddress();
                addressNumericValue nextAddress = 0;

                // Check if we have the next function address
                if (i < listOffsets.length() - 1)
                {
                    // Get the next function address
                    offsetIter.next();
                    nextAddress = (*offsetIter).getAddress();
                    offsetIter.prev();

                    // Ignore duplicates
                    if (address == nextAddress) continue;
                }

                subsetsFound = currMapper.map(address, nextAddress, true);
                prevSubsets = subsetsFound;
            }
        }
        XSTL_CATCH (cException& e)
        {
            cout << endl << "[!] Exception: " << e.getMessage() << " (" << e.getID() << ')' << endl;
            cout << "[!] Last opcode: " << HEXDWORD(currMapper.getLastOpcode().getAddress()) << endl;
            return false;
        }

        return true;
    }

#ifdef XSTL_WINDOWS
    struct PDBFunction {
        addressNumericValue m_start;
        addressNumericValue m_end;
        cString m_name;

        PDBFunction(addressNumericValue start,
                    addressNumericValue end,
                    cString& name) : m_start(start),
                                     m_end(end),
                                     m_name(name)
        {}
    };

    bool mapPDB(FlowMapper& currMapper, cList<PDBFunction>& listPDBFunctions)
    {
        uint subsetsFound = 0;
        uint prevSubsets = 0;

        // Sort the offsets by address
        cout << "[*] PDB functions to map: " << listPDBFunctions.length() << endl;

        XSTL_TRY
        {
            uint i = 0;
            for (cList<PDBFunction>::iterator pdbIter = listPDBFunctions.begin(); pdbIter != listPDBFunctions.end(); pdbIter++, i++)
            {
                addressNumericValue startAddress = (*pdbIter).m_start;
                addressNumericValue endAddress = (*pdbIter).m_end;

                subsetsFound = currMapper.map(startAddress, endAddress);
                prevSubsets = subsetsFound;
            }
        }
        XSTL_CATCH (cException& e)
        {
            cout << endl << "[!] Exception: " << e.getMessage() << " (" << e.getID() << ')' << endl;
            cout << "[!] Last opcode: " << HEXDWORD(currMapper.getLastOpcode().getAddress()) << endl;
            return false;
        }

        return true;
    }

    void loadListMap(FlowMapper& currMapper)
    {
        BasicInputPtr mapLoadFileStream(new cFileStream(OUTPUT_FILE, cFile::READ));
        currMapper.loadMapList(mapLoadFileStream);
        delete mapLoadFileStream;
        cList<FlowMapper::CodeSubset> listPrintMap;
        currMapper.getMapList(listPrintMap);
        cout << "[*] Read " << listPrintMap.length() << " subsets from file" << endl;
    }

    bool getPDBFunctionList(cString filename, cList<PDBFunction>& listPDBFunctions)
    {
        HRESULT hr = CoInitialize(NULL);

        CComPtr<IDiaDataSource> pSource;
        hr = CoCreateInstance(__uuidof(DiaSource),
                              NULL,
                              CLSCTX_INPROC_SERVER,
                              __uuidof(IDiaDataSource),
                              (void **)&pSource);
        if (FAILED(hr))
        {
            cout << "[!] ERROR: CoCreateInstance" << endl;
            return false;
        }

        if (FAILED(pSource->loadDataFromPdb(filename.getBuffer())))
        {
            if (FAILED(pSource->loadDataForExe(filename.getBuffer(), NULL, NULL)))
            {
                cout << "[!] ERROR: loadDataFromPdb/Exe" << endl;
                return false;
            }
        }

        CComPtr<IDiaSession> pSession;
        if (FAILED(pSource->openSession(&pSession)))
        {
            cout << "[!] ERROR: openSession" << endl;
            return false;
        }

        CComPtr<IDiaSymbol> pGlobal;
        if (FAILED(pSession->get_globalScope(&pGlobal)))
        {
            cout << "[!] ERROR: get_globalScope" << endl;
            return false;
        }

        CComPtr<IDiaEnumTables> pTables;
        if (FAILED(pSession->getEnumTables(&pTables)))
        {
            cout << "[!] ERROR: getEnumTables" << endl;
            return false;
        }

        ULONG celt = 0;

        IDiaEnumSymbols *pEnumSymbols;
        if (FAILED(pGlobal->findChildren(SymTagPublicSymbol, NULL, nsNone, &pEnumSymbols)))
        {
            cout << "[!] ERROR: findChildren" << endl;
            return false;
        }

        IDiaSymbol *pSymbol;
        for (; SUCCEEDED(pEnumSymbols->Next(1, &pSymbol, &celt)) && (celt == 1); pSymbol->Release())
        {
            DWORD dwSymTag;
            if (pSymbol->get_symTag(&dwSymTag) != S_OK)
                continue;

            if (10 != dwSymTag)
                continue;

            DWORD dwRVA;
            if (pSymbol->get_relativeVirtualAddress(&dwRVA) != S_OK)
                continue;

            ULONGLONG dwSize;
            if (pSymbol->get_length(&dwSize) != S_OK)
                continue;

            // Must be a function or a data symbol
            BSTR bstrName;
            if (pSymbol->get_name(&bstrName) != S_OK)
                continue;

            cString symbolName(bstrName);

            if (symbolName.find(cString("@")) == symbolName.length())
                continue;

            BSTR bstrUndname;
            if (pSymbol->get_undecoratedName(&bstrUndname) == S_OK)
            {
                cString undName(bstrUndname);
                SysFreeString(bstrUndname);
                if (undName != symbolName)
                    continue;
            }

            listPDBFunctions.append(PDBFunction(dwRVA, dwRVA + dwSize, symbolName));

            SysFreeString(bstrName);
        }
        pEnumSymbols->Release();
        return true;
    }
#endif

    void buildBPList(cList<FlowMapper::CodeSubset>& listMap, cList<addressNumericValue>& bpList)
    {
        cout << "[*] Building breakpoint list" << endl;

        uint32 numRets = 0, numCalls = 0, numJmps = 0;

        for (cList<FlowMapper::CodeSubset>::iterator iter = listMap.begin(); iter != listMap.end(); iter++)
        {
            int endAlterProperty = (*iter).m_endAlterProperty;
            int callerAlterProperty = (*iter).m_callerAlterProperty;

            // End address - Get only rets, and only those we can handle
            if ((Opcode::FLOW_RET | Opcode::FLOW_ACTION) == endAlterProperty)
            {
                if (!bpList.isIn((*iter).m_endAddress.getAddress()))
                {
                    bpList.append((*iter).m_endAddress.getAddress());
                    numRets++;
                }
            }

            // Caller address - Make sure we can handle the calling opcode
            if (Opcode::FLOW_ACTION & callerAlterProperty)
            {
                if (!bpList.isIn((*iter).m_callerAddress.getAddress()))
                {
                    bpList.append((*iter).m_callerAddress.getAddress());

                    if (Opcode::FLOW_STACK_CHANGE & callerAlterProperty)
                        numCalls++;
                    else if (Opcode::FLOW_COND_ALWAYS & callerAlterProperty)
                        numJmps++;
                }
            }
        }
    }

    bool testSingleFile(cString filename)
    {
        cout << "[*] Testing file: " << filename << endl;

        // Load a file
        BasicInputPtr peFileStream(new cFileStream(filename), SMARTPTR_DESTRUCT_NONE);
        addressNumericValue baseAddress = 0;
        //cout << "[*] Stream length: " << (uint32)peFileStream->length() << " bytes" << endl;

        // Read the PE
        cDosHeader dosFile(*peFileStream, true);
        XSTL_TRY
        {
            uint32 id;
            peFileStream->seek(dosFile.e_lfanew, basicInput::IO_SEEK_SET);
            peFileStream->pipeRead(&id ,sizeof(id));
            CHECK(id == IMAGE_NT_SIGNATURE);
            peFileStream->seek(dosFile.e_lfanew, basicInput::IO_SEEK_SET);
        }
        XSTL_CATCH_ALL
        {
            cout << "[!] Not a valid PE file!" << endl;
            return false;
        }
        cNtHeaderPtr ntFile(new cNtHeader(*peFileStream, 0));
        //cout << "[*] Expected image base: " << HEXDWORD(ntFile->OptionalHeader.ImageBase) << endl;
        //cout << "[*] Entry point: " << HEXDWORD(ntFile->OptionalHeader.AddressOfEntryPoint) << endl;

        // Build the memory interface object
        cList<cSectionPtr> ntSections;
        ntFile->getSections(ntSections);
        cList<SectionMemoryInterface::GeneralSection> generalSectionList;
        for (cList<cSectionPtr>::iterator i  = ntSections.begin(); i != ntSections.end(); ++i)
        {
            cNtSectionHeader& currSection = *((cNtSectionHeader*)(*i).getPointer());

            addressNumericValue start = currSection.VirtualAddress;
            addressNumericValue end = currSection.VirtualAddress + currSection.SizeOfRawData;
            addressNumericValue rawDataAddress = currSection.PointerToRawData;
            uint flags = currSection.getSectionFlags();
            generalSectionList.append(SectionMemoryInterface::GeneralSection(start, end, rawDataAddress, flags));
        }
        SectionMemoryInterfacePtr memoryInterface(new SectionMemoryInterface(baseAddress,
                                                                             ntFile->OptionalHeader.ImageBase,
                                                                             peFileStream->length(),
                                                                             generalSectionList),
                                                  SMARTPTR_DESTRUCT_NONE);

        // Read the reloc table
        cNtDirReloc relocDir(*ntFile);
        //cout << "[*] PE has " << relocDir.getRelocArray().getSize() << " relocations" << endl;
        cList<ProcessorAddress> listOffsets;
        cNtDirReloc::RelocTable relocArray = relocDir.getRelocArray();

        // Get the start and end addresses for the import table
        DWORD iatStartAddress = ntFile->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT].VirtualAddress;
        DWORD iatEndAddress = iatStartAddress + ntFile->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT].Size;

        // The difference between the actual PE address and the expected address
        int diff = baseAddress - ntFile->OptionalHeader.ImageBase;

        for (uint i = 0; i < relocArray.getSize(); i++)
        {
            addressNumericValue address = relocArray[i].m_offsetAddress.getAddressValue();

            // Read the relocation address content, and create a relocated address object
            uint32 relocContent;
            peFileStream->seek(memoryInterface->virtualToRawAddress(address), basicInput::IO_SEEK_SET);
            peFileStream->streamReadUint32(relocContent);
            ProcessorAddress offsetAddress(ProcessorAddress::PROCESSOR_32, relocContent + diff);

            // Check if the offset falls inside the import table
            if ((offsetAddress.getAddress() >= iatStartAddress) &&
                (offsetAddress.getAddress() <= iatEndAddress))
                continue;

            // If the offset is in the code section - add it to the list
            if (memoryInterface->checkAddress(offsetAddress.getAddress(), SectionMemoryInterface::SECTION_FLAG_EXECUTABLE))
                if (!listOffsets.isIn(offsetAddress))
                    listOffsets.append(offsetAddress);
        }
        //cout << "[*] Got " << (uint32)listOffsets.length() << " offsets from relocations" << endl;

        // Read the export-table
        cNtDirExport exportDir(*ntFile);
        //cout << "[*] PE has " << (uint32)exportDir.getExportArray().getSize() << " exports" << endl;

        // Seek to the start of the file and create a flow mapper object
        peFileStream->seek(0, basicInput::IO_SEEK_SET);
        FlowMapper currMapper(peFileStream, memoryInterface);
        //cout << "[*] Initialized flow mapper object" << endl;

        if (!mapExports(currMapper, exportDir.getExportArray()))
        {
            cout << "[!] Failed running exports" << endl;
            return false;
        }

        if (!mapOffsets(currMapper, listOffsets))
        {
            cout << "[!] Failed running offsets" << endl;
            return false;
        }

        cList<FlowMapper::CodeSubset> listMap;
        currMapper.getMapList(listMap);
        //cout << "[*] Total subsets found: " << listMap.length() << endl;

        return true;
    }

    virtual void test_single_file()
    {
        cout << "[*] Started TestFlowMap::test() on list: " << LIST_FILE_PATH << endl;
        cFileStream inputFile(LIST_FILE_PATH);
        cString fileData = inputFile.readFixedSizeString(inputFile.length(), 1);
        cList<cString> fileLines = fileData.split(cString("\r\n"));
        for(cList<cString>::iterator i = fileLines.begin(); i != fileLines.end(); i++)
        {
            XSTL_TRY
            {
                if (!testSingleFile((*i)))
                {
                    cout << "[!] Filename: " << (*i) << endl;
                    break;
                }
            }
            XSTL_CATCH_ALL
            { }
        }
    }

    virtual void test()
    {
        cString filename = FILE_PATH;
        cout << "[*] Started TestFlowMap::test() on " << filename << endl;

        /*
        #ifdef XSTL_WINDOWS
        // Read the PDB function data
        cList<PDBFunction> pdbFunctions;
        getPDBFunctionList(PDB_FILENAME, pdbFunctions);
        cout << "[*] Read " << pdbFunctions.length() << " public symbols from PDB" << endl;
        return;
        #endif
        */

        // Load a file
        BasicInputPtr peFileStream(new cFileStream(filename), SMARTPTR_DESTRUCT_NONE);
        addressNumericValue baseAddress = 0;
        cout << "[*] Stream length: " << (uint32)peFileStream->length() << " bytes" << endl;

        // Read the PE
        cDosHeader dosFile(*peFileStream, true);
        XSTL_TRY
        {
            uint32 id;
            peFileStream->seek(dosFile.e_lfanew, basicInput::IO_SEEK_SET);
            peFileStream->pipeRead(&id ,sizeof(id));
            CHECK(id == IMAGE_NT_SIGNATURE);
            peFileStream->seek(dosFile.e_lfanew, basicInput::IO_SEEK_SET);
        }
        XSTL_CATCH_ALL
        {
            cout << "[!] Not a valid PE file!" << endl;
            return;
        }
        cNtHeaderPtr ntFile(new cNtHeader(*peFileStream, 0));
        cout << "[*] Expected image base: " << HEXDWORD(ntFile->OptionalHeader.ImageBase) << endl;
        cout << "[*] Entry point: " << HEXDWORD(ntFile->OptionalHeader.AddressOfEntryPoint) << endl;

        // Build the memory interface object
        cList<cSectionPtr> ntSections;
        ntFile->getSections(ntSections);
        cList<SectionMemoryInterface::GeneralSection> generalSectionList;
        for (cList<cSectionPtr>::iterator i  = ntSections.begin(); i != ntSections.end(); ++i)
        {
            cNtSectionHeader& currSection = *((cNtSectionHeader*)(*i).getPointer());

            addressNumericValue start = currSection.VirtualAddress;
            addressNumericValue end = currSection.VirtualAddress + currSection.SizeOfRawData;
            addressNumericValue rawDataAddress = currSection.PointerToRawData;
            uint flags = currSection.getSectionFlags();
            generalSectionList.append(SectionMemoryInterface::GeneralSection(start, end, rawDataAddress, flags));
        }
        SectionMemoryInterfacePtr memoryInterface(new SectionMemoryInterface(baseAddress,
                                                                             ntFile->OptionalHeader.ImageBase,
                                                                             peFileStream->length(),
                                                                             generalSectionList),
                                                  SMARTPTR_DESTRUCT_NONE);

        // Read the reloc table
        cNtDirReloc relocDir(*ntFile);
        //cout << "[*] PE has " << relocDir.getRelocArray().getSize() << " relocations" << endl;
        cList<ProcessorAddress> listOffsets;
        cNtDirReloc::RelocTable relocArray = relocDir.getRelocArray();

        // Get the start and end addresses for the import table
        DWORD iatStartAddress = ntFile->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT].VirtualAddress;
        DWORD iatEndAddress = iatStartAddress + ntFile->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT].Size;

        // The difference between the actual PE address and the expected address
        int diff = baseAddress - ntFile->OptionalHeader.ImageBase;

        for (uint i = 0; i < relocArray.getSize(); i++)
        {
            addressNumericValue address = relocArray[i].m_offsetAddress.getAddressValue();

            // Read the relocation address content, and create a relocated address object
            uint32 relocContent;
            peFileStream->seek(memoryInterface->virtualToRawAddress(address), basicInput::IO_SEEK_SET);
            peFileStream->streamReadUint32(relocContent);
            ProcessorAddress offsetAddress(ProcessorAddress::PROCESSOR_32, relocContent + diff);

            // Check if the offset falls inside the import table
            if ((offsetAddress.getAddress() >= iatStartAddress) &&
                (offsetAddress.getAddress() <= iatEndAddress))
                continue;

            // If the offset is in the code section - add it to the list
            if (memoryInterface->checkAddress(offsetAddress.getAddress(), SectionMemoryInterface::SECTION_FLAG_EXECUTABLE))
                if (!listOffsets.isIn(offsetAddress))
                    listOffsets.append(offsetAddress);
        }
        //cout << "[*] Got " << (uint32)listOffsets.length() << " offsets from relocations" << endl;

        // Read the export-table
        cNtDirExport exportDir(*ntFile);
        //cout << "[*] PE has " << (uint32)exportDir.getExportArray().getSize() << " exports" << endl;

        // Seek to the start of the file and create a flow mapper object
        peFileStream->seek(0, basicInput::IO_SEEK_SET);
        FlowMapper currMapper(peFileStream, memoryInterface);
        cout << "[*] Initialized flow mapper object" << endl;

        if (!mapExports(currMapper, exportDir.getExportArray())) return;

        if (!mapOffsets(currMapper, listOffsets)) return;

        cList<FlowMapper::CodeSubset> listMap;
        currMapper.getMapList(listMap);
        cout << "[*] Total subsets found: " << listMap.length() << endl;

        cList<addressNumericValue> bpList;
        buildBPList(listMap, bpList);
        cout << "[*] Breakpoints: " << bpList.length() << endl;

        /*
        BasicOutputPtr mapDumpFileStream(new cFileStream(OUTPUT_FILE, cFile::CREATE | cFile::WRITE), SMARTPTR_DESTRUCT_NONE);
        currMapper.dumpMapList(mapDumpFileStream);
        mapDumpFileStream = BasicOutputPtr(NULL);
        cout << "[*] Wrote map list to file" << endl;
        */
    }

    // Return the name of the module
    virtual cString getName() { return __FILE__; }
};

// Instance test object
TestObjectTestFlowMap g_globalTestFlowMap;
