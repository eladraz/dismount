INCLUDES = -I $(DISMOUNT_PATH)/Include -I $(XSTL_PATH)/Include -I $(PELIB_PATH)/Include

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

test_dismount_SOURCES = TestIA32AssemblerDisassembler.cpp $(XSTL_PATH)/tests/tests.cpp $(PETESTS)

test_dismount_CFLAGS = $(CFLAGS_DISMOUNT_COMMON) $(DBGFLAGS) $(AM_CFLAGS)
test_dismount_CPPFLAGS = $(CFLAGS_DISMOUNT_COMMON) $(DBGFLAGS) $(AM_CFLAGS)

if UNICODE
test_dismount_CFLAGS+= -DXSTL_UNICODE -D_UNICODE
test_dismount_CPPFLAGS+= -DXSTL_UNICODE -D_UNICODE
endif

test_dismount_LDADD = -L$(XSTL_PATH)/out/lib -lxstl \
                      -L$(XSTL_PATH)/out/lib -lxstl_data \
                      -L$(XSTL_PATH)/out/lib -lxstl_except \
                      -L$(XSTL_PATH)/out/lib -lxstl_stream \
                      -L$(XSTL_PATH)/out/lib -lxstl_os \
                      -L$(XSTL_PATH)/out/lib -lxstl_unix \
                      -L$(XSTL_PATH)/out/lib -lxstl_enc \
                      -L$(XSTL_PATH)/out/lib -lxstl_digest \
                      -L$(XSTL_PATH)/out/lib -lxstl_random \
                      -L$(XSTL_PATH)/out/lib -lxstl_encryptions \
                      -L$(XSTL_PATH)/out/lib -lxstl_utils \
                      $(LIBFLAGS) \
                      -L$(top_srcdir) -ldismount

