cmake_minimum_required (VERSION 2.6...3.5)
project (dismount)
set (dismount_VERSION_MAJOR 1)
set (dismount_VERSION_MINOR 0)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -fPIC  -Wno-write-strings")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -fPIC  -Wno-write-strings")

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/out/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/out/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/out/bin)

if(NOT CMAKE_BUILD_TYPE)
	message(WARNING "cmake was execute without specify build type, run cmake -DCMAKE_BUILD_TYPE=Release for release build")
	set(CMAKE_BUILD_TYPE Debug)
endif()

if(CMAKE_BUILD_TYPE MATCHES Debug)
	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -ggdb -DDEBUG")
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ggdb -DDEBUG")
endif()

if(CMAKE_BUILD_TYPE MATCHES RelWithDebInfo)
	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g")
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g")
endif()

include_directories($ENV{XSTL_PATH}/Include)
include_directories(Include)

add_definitions(-DXSTL_UNICODE)
add_definitions(-D_UNICODE)

list(APPEND DISMOUNT_LIB_FILES
	Source/dismount/DefaultOpcodeDataFormatter.cpp
	Source/dismount/InvalidOpcodeByte.cpp
	Source/dismount/OpcodeSubsystems.cpp
	Source/dismount/dismount.cpp
	Source/dismount/InvalidOpcodeFormatter.cpp
	Source/dismount/ProcessorAddress.cpp
	Source/dismount/FlowMapper.cpp
	Source/dismount/OpcodeFormatter.cpp
	Source/dismount/StreamDisassemblerFactory.cpp
	Source/dismount/FlowMapperException.cpp
	Source/dismount/SectionMemoryInterface.cpp
	Source/dismount/assembler/AssemblingFactory.cpp
	Source/dismount/assembler/MangledNames.cpp
	Source/dismount/assembler/StackInterface.cpp
	Source/dismount/assembler/BinaryDependencies.cpp
	Source/dismount/assembler/SecondPassBinary.cpp
	Source/dismount/assembler/FirstPassBinary.cpp
	Source/dismount/assembler/SecondPassInfoAndDebug.cpp
	Source/dismount/assembler/DependencyException.cpp
	Source/dismount/assembler/proc/ia32/IA32Assembler.cpp
	Source/dismount/proc/ia32/IA32IntelNotation.cpp
	Source/dismount/proc/ia32/IA32StreamDisassembler.cpp
	Source/dismount/proc/ia32/IA32Opcode.cpp
	Source/dismount/proc/ia32/opcodeTable.cpp
	Source/dismount/proc/ia32/opcodeTableAMD64.cpp
	Source/dismount/semantics/SemanticType.cpp
	Source/dismount/semantics/SemanticOperation.cpp
	Source/dismount/semantics/SemanticOperand.cpp
	Source/dismount/semantics/SemanticInstruction.cpp
	Source/dismount/semantics/SemanticBlock.cpp
	Source/dismount/semantics/OpcodeClassification.cpp
	Source/dismount/semantics/ArchitectureDescription.cpp
	Source/dismount/semantics/SemanticFloat.cpp
	Source/dismount/semantics/SemanticInterpreter.cpp
	Source/dismount/semantics/SemanticLifter.cpp
	Source/dismount/proc/ia32/IA32Architecture.cpp
	Source/dismount/proc/ia32/AMD64Architecture.cpp
	Source/dismount/proc/ia32/IA32OperandDecoder.cpp
	Source/dismount/proc/ia32/IA32Semantics.cpp
	Source/dismount/proc/ia32/IA32SemanticTable.cpp
	Source/dismount/proc/ia32/IA32SemanticTableSegments.cpp
	Source/dismount/proc/ia32/IA32SemanticTableAMD64.cpp
	Source/dismount/proc/ia32/IA32SemanticTableX87.cpp
	Source/dismount/proc/ia32/IA32SemanticTableSimd.cpp
	# ARM: the architecture descriptions of the semantic layer
	Source/dismount/proc/arm/ArmArchitecture.cpp
	Source/dismount/proc/arm/Arm64Architecture.cpp
	Source/dismount/proc/arm/ArmSemantics.cpp
	Source/dismount/proc/arm/ArmSemanticTable.cpp
	Source/dismount/proc/arm/ArmSemanticLift32.cpp
	Source/dismount/proc/arm/ArmSemanticLift32Memory.cpp
	Source/dismount/proc/arm/ArmSemanticLiftVfp.cpp
	Source/dismount/proc/arm/ArmSemanticLiftNeon.cpp
	Source/dismount/proc/arm/ArmSemanticLiftNeonMemory.cpp
	Source/dismount/proc/arm/ArmSemanticLiftCrypto.cpp
	Source/dismount/proc/arm/ArmSemanticTableSimd.cpp
	Source/dismount/proc/arm/Arm64SemanticTable.cpp
	Source/dismount/proc/arm/Arm64SemanticLift.cpp
	Source/dismount/proc/arm/Arm64SemanticLiftMemory.cpp
	Source/dismount/proc/arm/Arm64SemanticSveHelpers.cpp
	Source/dismount/proc/arm/Arm64SemanticLiftSve.cpp
	Source/dismount/proc/arm/Arm64SemanticLiftSvePredicate.cpp
	Source/dismount/proc/arm/Arm64SemanticLiftSveMemory.cpp
	Source/dismount/proc/arm/Arm64SemanticLiftSveFloat.cpp
	Source/dismount/proc/arm/Arm64SemanticLiftSve2.cpp
	Source/dismount/proc/arm/Arm64SemanticLiftSve2Wide.cpp
	Source/dismount/proc/arm/Arm64SemanticLiftSveFloat2.cpp
	Source/dismount/proc/arm/Arm64SemanticLiftSveQuad.cpp
	Source/dismount/proc/arm/Arm64SemanticLiftSveCrypto.cpp
	Source/dismount/proc/arm/Arm64SemanticLiftSveMisc.cpp
	Source/dismount/proc/arm/Arm64SemanticTableSve.cpp
	Source/dismount/proc/arm/Arm64SemanticLiftSimd.cpp
	Source/dismount/proc/arm/Arm64SemanticLiftSimdInteger.cpp
	Source/dismount/proc/arm/Arm64SemanticLiftFloat.cpp
	Source/dismount/proc/arm/Arm64SemanticLiftSimdMemory.cpp
	Source/dismount/proc/arm/Arm64SemanticLiftCrypto.cpp
	Source/dismount/proc/arm/Arm64SemanticTableSimd.cpp
	Source/dismount/proc/arm/Arm64SemanticTableSme.cpp
	Source/dismount/proc/arm/Arm64SemanticLiftSme.cpp
	Source/dismount/proc/arm/Arm64SemanticLiftSmeOuter.cpp
	Source/dismount/proc/arm/Arm64SemanticLiftSmeArray.cpp
	Source/dismount/proc/arm/Arm64SemanticLiftSmeVector.cpp
	Source/dismount/proc/arm/Arm64SemanticLiftSmeConvert.cpp
	Source/dismount/assembler/proc/arm/ArmAssembler.cpp
	Source/dismount/assembler/proc/arm/ArmAssemblerA32.cpp
	Source/dismount/proc/arm/ArmInstructionSet.cpp
	# RISC-V: the architecture description and the semantic layer
	Source/dismount/proc/riscv/RiscvArchitecture.cpp
	Source/dismount/proc/riscv/RiscvSemantics.cpp
	Source/dismount/proc/riscv/RiscvSemanticTable.cpp
	Source/dismount/proc/riscv/RiscvSemanticTableStubs.cpp
	Source/dismount/proc/riscv/RiscvSemanticTablePrivileged.cpp
	Source/dismount/proc/riscv/RiscvSemanticLiftCommon.cpp
	Source/dismount/proc/riscv/RiscvSemanticLiftInteger.cpp
	Source/dismount/proc/riscv/RiscvSemanticLiftFloat.cpp
	Source/dismount/proc/riscv/RiscvSemanticLiftAtomic.cpp
	Source/dismount/proc/riscv/RiscvSemanticLiftSystem.cpp
	Source/dismount/proc/riscv/RiscvSemanticLiftCrypto.cpp
	Source/dismount/proc/riscv/RiscvHelpers.cpp
	Source/dismount/proc/arm/ArmFeatures.cpp
	Source/dismount/proc/arm/ArmOpcode.cpp
	Source/dismount/proc/arm/ArmOperandCodec.cpp
	Source/dismount/proc/arm/ArmOperandCodecA32.cpp
	Source/dismount/proc/arm/ArmOperandCodecA32Memory.cpp
	Source/dismount/proc/arm/ArmOperandCodecA32Thumb.cpp
	Source/dismount/proc/arm/ArmOperandCodecA32Simd.cpp
	Source/dismount/proc/arm/ArmNotationA32.cpp
	Source/dismount/proc/arm/ArmNotationA32Simd.cpp
	Source/dismount/proc/arm/ArmOperandCodecA64.cpp
	Source/dismount/proc/arm/ArmOperandCodecSve.cpp
	Source/dismount/proc/arm/ArmOperandCodecSme.cpp
	Source/dismount/proc/arm/ArmOperandCodecSimd.cpp
	Source/dismount/proc/arm/ArmOperandNames.cpp
	Source/dismount/proc/arm/Arm64Names.cpp
	Source/dismount/proc/arm/ArmNotation.cpp
	Source/dismount/proc/arm/ArmStreamDisassembler.cpp
	Source/dismount/proc/arm/Arm64EncodingTable.cpp
	Source/dismount/proc/arm/Arm64EncodingTableSimd.cpp
	Source/dismount/proc/arm/Arm64EncodingTableSve.cpp
	Source/dismount/proc/arm/Arm64EncodingTableSme.cpp
	Source/dismount/proc/arm/ArmEncodingTableA32.cpp
	Source/dismount/proc/arm/ArmEncodingTableA32Simd.cpp
	Source/dismount/proc/arm/ArmEncodingTableT16.cpp
	Source/dismount/proc/arm/ArmEncodingTableT32.cpp
	Source/dismount/proc/riscv/RiscvInstructionSet.cpp
	Source/dismount/proc/riscv/RiscvFeatures.cpp
	Source/dismount/proc/riscv/RiscvCsrNames.cpp
	Source/dismount/proc/riscv/RiscvOperandCodec.cpp
	Source/dismount/proc/riscv/RiscvOperandCodecCompressed.cpp
	Source/dismount/proc/riscv/RiscvOperandCodecVector.cpp
	Source/dismount/proc/riscv/RiscvEncodingTable.cpp
	Source/dismount/proc/riscv/RiscvEncodingTableCompressed.cpp
	Source/dismount/proc/riscv/RiscvEncodingTablePrivileged.cpp
	Source/dismount/proc/riscv/RiscvEncodingTableVector.cpp
	Source/dismount/proc/riscv/RiscvEncodingTableVectorMemory.cpp
	Source/dismount/proc/riscv/RiscvEncodingTableVectorExtension.cpp
	Source/dismount/proc/riscv/RiscvOpcode.cpp
	Source/dismount/proc/riscv/RiscvNotation.cpp
	Source/dismount/proc/riscv/RiscvStreamDisassembler.cpp
	Source/dismount/assembler/proc/riscv/RiscvAssembler.cpp
	Source/dismount/assembler/proc/riscv/RiscvAssemblerPseudo.cpp
)

add_library(dismount_static STATIC ${DISMOUNT_LIB_FILES})
add_library(dismount SHARED ${DISMOUNT_LIB_FILES})

# Try to find CMake single library
	find_library(LIBXSTLCM xStlcm HINTS $ENV{XSTL_PATH}/out/lib)
	target_link_libraries(dismount ${LIBXSTLCM})
if(NOT LIBXSTLCM)
# Compatibility mode with autotools (Only linux) isn't fully tested.
	find_library(LIBXSTL xstl HINTS $ENV{XSTL_PATH}/out/lib)
	find_library(LIBXSTL_DATA xstl_data HINTS $ENV{XSTL_PATH}/out/lib)
	find_library(LIBXSTL_EXCEPT xstl_except HINTS $ENV{XSTL_PATH}/out/lib)
	find_library(LIBXSTL_OS xstl_os HINTS $ENV{XSTL_PATH}/out/lib)
	find_library(LIBXSTL_PARSER xstl_parser HINTS $ENV{XSTL_PATH}/out/lib)
	find_library(LIBXSTL_STREAM xstl_stream HINTS $ENV{XSTL_PATH}/out/lib)
	find_library(LIBXSTL_UNIX xstl_unix HINTS $ENV{XSTL_PATH}/out/lib)

	target_link_libraries(dismount ${LIBXSTL} ${LIBXSTL_DATA} ${LIBXSTL_EXCEPT}
	                               ${LIBXSTL_OS} ${LIBXSTL_PARSER} ${LIBXSTL_STREAM}
	                               ${LIBXSTL_UNIX})
endif()
if (UNIX)
	set(CMAKE_MACOSX_RPATH 1)
	add_definitions(-DLINUX)
endif()
if (WIN32)
	add_definitions(-DWIN32)
endif()

# Test programs: `dismount_cli` (tests/ia32, used by run_compat.py) and
# `test_dismount` (the xStl harness unit test). Off by default.
option(DISMOUNT_BUILD_TESTS "Build dismount_cli and test_dismount" OFF)
if(DISMOUNT_BUILD_TESTS)
	find_package(Threads)
	if(NOT LIBXSTLCM)
		message(FATAL_ERROR "DISMOUNT_BUILD_TESTS needs the CMake xStl library (libxStlcm)")
	endif()
	set(DISMOUNT_TEST_LIBS dismount_static ${LIBXSTLCM} ${CMAKE_THREAD_LIBS_INIT})

	add_executable(dismount_cli tests/ia32/dismount_cli.cpp)
	target_link_libraries(dismount_cli ${DISMOUNT_TEST_LIBS})

	add_executable(test_dismount
		tests/TestIA32AssemblerDisassembler.cpp
		tests/TestAMD64Disassembler.cpp
		tests/TestAMD64Assembler.cpp
		tests/TestArm64Disassembler.cpp
		tests/TestRiscvDisassembler.cpp
		tests/TestRiscvVector.cpp
		tests/TestArm64Sve.cpp
		tests/TestArm64Sme.cpp
		tests/TestArm64Simd.cpp
		tests/TestArmAArch32Disassembler.cpp
		tests/TestSemantics.cpp
		tests/TestAMD64Semantics.cpp
		tests/TestSemanticFloat.cpp
		tests/TestArmArchitecture.cpp
		tests/TestRiscvArchitecture.cpp
		tests/TestRiscvSemantics.cpp
		tests/TestSemanticFloatRiscv.cpp
		tests/TestArm64Semantics.cpp
		tests/TestArm64SemanticsSve.cpp
		tests/TestArm64SemanticsSimd.cpp
		tests/TestArm64SemanticsSme.cpp
		tests/TestSemanticFloatArm.cpp
		tests/TestArmAArch32Semantics.cpp
		$ENV{XSTL_PATH}/tests/tests.cpp)
	target_link_libraries(test_dismount ${DISMOUNT_TEST_LIBS})
endif()
