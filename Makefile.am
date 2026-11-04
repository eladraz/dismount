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
                         Source/dismount/proc/ia32/opcodeTable.cpp              \
                         Source/dismount/proc/ia32/opcodeTableAMD64.cpp         \
                         Source/dismount/semantics/SemanticType.cpp             \
                         Source/dismount/semantics/SemanticOperation.cpp        \
                         Source/dismount/semantics/SemanticOperand.cpp          \
                         Source/dismount/semantics/SemanticInstruction.cpp      \
                         Source/dismount/semantics/SemanticBlock.cpp            \
                         Source/dismount/semantics/OpcodeClassification.cpp     \
                         Source/dismount/semantics/ArchitectureDescription.cpp  \
                         Source/dismount/semantics/SemanticFloat.cpp            \
                         Source/dismount/semantics/SemanticInterpreter.cpp      \
                         Source/dismount/semantics/SemanticLifter.cpp           \
                         Source/dismount/proc/ia32/IA32Architecture.cpp         \
                         Source/dismount/proc/ia32/AMD64Architecture.cpp        \
                         Source/dismount/proc/ia32/IA32OperandDecoder.cpp       \
                         Source/dismount/proc/ia32/IA32Semantics.cpp            \
                         Source/dismount/proc/ia32/IA32SemanticTable.cpp        \
                         Source/dismount/proc/ia32/IA32SemanticTableSegments.cpp \
                         Source/dismount/proc/ia32/IA32SemanticTableAMD64.cpp   \
                         Source/dismount/proc/ia32/IA32SemanticTableX87.cpp     \
                         Source/dismount/proc/ia32/IA32SemanticTableSimd.cpp \
                         Source/dismount/proc/arm/ArmArchitecture.cpp           \
                         Source/dismount/proc/arm/Arm64Architecture.cpp \
                         Source/dismount/proc/arm/ArmSemantics.cpp \
                         Source/dismount/proc/arm/ArmSemanticTable.cpp \
                         Source/dismount/proc/arm/ArmSemanticLift32.cpp \
                         Source/dismount/proc/arm/ArmSemanticLift32Memory.cpp \
                         Source/dismount/proc/arm/ArmSemanticLiftVfp.cpp \
                         Source/dismount/proc/arm/ArmSemanticLiftNeon.cpp \
                         Source/dismount/proc/arm/ArmSemanticLiftNeonMemory.cpp \
                         Source/dismount/proc/arm/ArmSemanticLiftCrypto.cpp \
                         Source/dismount/proc/arm/ArmSemanticTableSimd.cpp \
                         Source/dismount/proc/arm/ArmSemanticTableStubs.cpp \
                         Source/dismount/proc/arm/Arm64SemanticTable.cpp \
                         Source/dismount/proc/arm/Arm64SemanticLift.cpp \
                         Source/dismount/proc/arm/Arm64SemanticLiftMemory.cpp \
                         Source/dismount/assembler/proc/arm/ArmAssembler.cpp \
                         Source/dismount/assembler/proc/arm/ArmAssemblerA32.cpp \
                         Source/dismount/proc/arm/ArmInstructionSet.cpp \
                         Source/dismount/proc/arm/ArmFeatures.cpp \
                         Source/dismount/proc/arm/ArmOpcode.cpp \
                         Source/dismount/proc/arm/ArmOperandCodec.cpp \
                         Source/dismount/proc/arm/ArmOperandCodecA32.cpp \
                         Source/dismount/proc/arm/ArmOperandCodecA32Memory.cpp \
                         Source/dismount/proc/arm/ArmOperandCodecA32Thumb.cpp \
                         Source/dismount/proc/arm/ArmOperandCodecA32Simd.cpp \
                         Source/dismount/proc/arm/ArmNotationA32.cpp \
                         Source/dismount/proc/arm/ArmNotationA32Simd.cpp \
                         Source/dismount/proc/arm/ArmOperandCodecA64.cpp \
                         Source/dismount/proc/arm/ArmOperandCodecSve.cpp \
                         Source/dismount/proc/arm/ArmOperandCodecSimd.cpp \
                         Source/dismount/proc/arm/ArmOperandNames.cpp \
                         Source/dismount/proc/arm/Arm64Names.cpp \
                         Source/dismount/proc/arm/ArmNotation.cpp \
                         Source/dismount/proc/arm/ArmStreamDisassembler.cpp \
                         Source/dismount/proc/arm/Arm64EncodingTable.cpp \
                         Source/dismount/proc/arm/Arm64EncodingTableSimd.cpp \
                         Source/dismount/proc/arm/Arm64EncodingTableSve.cpp \
                         Source/dismount/proc/arm/Arm64EncodingTableSme.cpp \
                         Source/dismount/proc/arm/ArmEncodingTableA32.cpp \
                         Source/dismount/proc/arm/ArmEncodingTableA32Simd.cpp \
                         Source/dismount/proc/arm/ArmEncodingTableT16.cpp \
                         Source/dismount/proc/arm/ArmEncodingTableT32.cpp



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

