cmake_minimum_required (VERSION 2.6)
project (dismount)
set (dismount_VERSION_MAJOR 1)
set (dismount_VERSION_MINOR 0)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -fPIC  -Wno-write-strings")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -fPIC  -Wno-write-strings")

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/out/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/out/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/out/bin)

set(CMAKE_BUILD_TYPE Debug)
set(CMAKE_BUILD_TYPE Release)
set(CMAKE_BUILD_TYPE RelWithDebInfo)

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
