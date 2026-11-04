#!/bin/sh
# Build the library and the test programs of the build/ directory (made by `mkdir build; cd build;
# cmake .. -DCMAKE_BUILD_TYPE=Debug -DDISMOUNT_BUILD_TESTS=ON`); print only the errors.
# XSTL_PATH defaults to /Users/razi/work/xStl.
export XSTL_PATH=${XSTL_PATH:-/Users/razi/work/xStl}
cd "$(dirname "$0")/../../../../build" || exit 1
make -j10 2>&1 | grep -E "error|Error"
exit 0
