INCLUDES = -I $(top_srcdir)/Include -I $(XSTL_PATH)/Include -I $(PELIB_PATH)/Include

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g
endif

lib_LTLIBRARIES = libdismount.la

libdismount_la_SOURCES = Source/dismount/DefaultOpcodeDataFormatter.cpp         \
                         Source/dismount/InvalidOpcodeByte.cpp                  \
                         Source/dismount/OpcodeSubsystems.cpp                   \
                         Source/dismount/dismount.cpp                           \
                         Source/dismount/InvalidOpcodeFormatter.cpp             \
                         Source/dismount/ProcessorAddress.cpp                   \
                         Source/dismount/FlowMapper.cpp                         \
                         Source/dismount/OpcodeFormatter.cpp                    \
                         Source/dismount/StreamDisassemblerFactory.cpp          \
                         Source/dismount/FlowMapperException.cpp                \
                         Source/dismount/SectionMemoryInterface.cpp             \
                         Source/dismount/assembler/AssemblingFactory.cpp        \
                         Source/dismount/assembler/MangledNames.cpp             \
                         Source/dismount/assembler/StackInterface.cpp           \
                         Source/dismount/assembler/BinaryDependencies.cpp       \
                         Source/dismount/assembler/SecondPassBinary.cpp         \
                         Source/dismount/assembler/FirstPassBinary.cpp          \
                         Source/dismount/assembler/SecondPassInfoAndDebug.cpp   \
                         Source/dismount/assembler/DependencyException.cpp      \
                         Source/dismount/assembler/proc/ia32/IA32Assembler.cpp  \
                         Source/dismount/proc/ia32/IA32IntelNotation.cpp        \
                         Source/dismount/proc/ia32/IA32StreamDisassembler.cpp   \
                         Source/dismount/proc/ia32/IA32Opcode.cpp               \
                         Source/dismount/proc/ia32/opcodeTable.cpp



libdismount_la_CFLAGS = $(CFLAGS_DISMOUNT_COMMON) $(DBGFLAGS) $(AM_CFLAGS)
libdismount_la_CPPFLAGS = $(CFLAGS_DISMOUNT_COMMON) $(DBGFLAGS) $(AM_CFLAGS)

if UNICODE
libdismount_la_CFLAGS+= -DXSTL_UNICODE -D_UNICODE
libdismount_la_CPPFLAGS+= -DXSTL_UNICODE -D_UNICODE
endif

if TESTS
SUBDIRS = . tests
endif

DIST_SUBDIRS = ${SUBDIRS}

