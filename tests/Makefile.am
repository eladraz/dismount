INCLUDES = -I ../Include -I $(XSTL_PATH)/Include -I $(PELIB_PATH)/Include

if DEBUG
DBGFLAGS = -ggdb -DDEBUG
else
DBGFLAGS = -g
endif

if PELIB
LIBFLAGS = -L$(PELIB_PATH)/out/lib -lpe
PETESTS = testFlowMap.cpp
else
LIBFLAGS =
PETESTS =
endif

bin_PROGRAMS = test_dismount

test_dismount_SOURCES = TestIA32AssemblerDisassembler.cpp TestAMD64Disassembler.cpp TestArm64Disassembler.cpp TestArm64Simd.cpp TestArm64Sve.cpp TestArmAArch32Disassembler.cpp TestSemantics.cpp TestAMD64Semantics.cpp TestSemanticFloat.cpp TestArmArchitecture.cpp TestArm64Semantics.cpp $(XSTL_PATH)/tests/tests.cpp $(PETESTS)

test_dismount_CFLAGS = $(CFLAGS_DISMOUNT_COMMON) $(DBGFLAGS) $(AM_CFLAGS)
test_dismount_CPPFLAGS = $(CFLAGS_DISMOUNT_COMMON) $(DBGFLAGS) $(AM_CFLAGS)

if UNICODE
test_dismount_CFLAGS+= -DXSTL_UNICODE -D_UNICODE
test_dismount_CPPFLAGS+= -DXSTL_UNICODE -D_UNICODE
endif

# With --as-needed (the default of gcc on Ubuntu) the linker keeps a library
# only if an object or a library before it uses it. So libdismount comes
# first, and every xStl library follows a library that uses it: libxstl is
# used by the xStl libraries only.
test_dismount_LDADD = -L$(top_srcdir) -ldismount \
                      -L$(XSTL_PATH)/out/lib -lxstl_parser \
                      -L$(XSTL_PATH)/out/lib -lxstl_data \
                      -L$(XSTL_PATH)/out/lib -lxstl_except \
                      -L$(XSTL_PATH)/out/lib -lxstl_stream \
                      -L$(XSTL_PATH)/out/lib -lxstl_os \
                      -L$(XSTL_PATH)/out/lib -lxstl_unix \
                      -L$(XSTL_PATH)/out/lib -lxstl \
                      -L$(XSTL_PATH)/out/lib -lxstl_enc \
                      -L$(XSTL_PATH)/out/lib -lxstl_utils \
                      -L$(XSTL_PATH)/out/lib -lxstl_random \
                      -L$(XSTL_PATH)/out/lib -lxstl_digest \
                      -L$(XSTL_PATH)/out/lib -lxstl_encryptions \
                      $(LIBFLAGS)
