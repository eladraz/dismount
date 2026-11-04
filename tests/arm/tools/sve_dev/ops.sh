#!/bin/sh
# usage: ops.sh file-with-instruction-lines : dumps the decoded operands (opdump.cpp) of each line
here="$(cd "$(dirname "$0")" && pwd)"
root="$here/../../../.."
cd "$root" || exit 1
X=${XSTL_PATH:-/Users/razi/work/xStl}
mkdir -p out/scratch
c++ -DLINUX -DXSTL_UNICODE -D_UNICODE -DDEBUG -I$X/Include -IInclude -Wno-write-strings tests/arm/tools/sve_dev/opdump.cpp out/lib/libdismount_static.a $X/out/lib/libxStlcm.dylib -Wl,-rpath,$X/out/lib -o out/scratch/opdump 2>&1 | grep error
python3 tests/arm/tools/sve_dev/asmwords.py < "$1" | while read w c t; do echo "== $t"; echo $w | out/scratch/opdump; done
