#!/bin/sh
# usage: ir.sh VL file-with-instruction-lines : dumps the semantic IR the lifter makes for each line
# (builds irdump.cpp against out/lib). XSTL_PATH defaults to /Users/razi/work/xStl.
here="$(cd "$(dirname "$0")" && pwd)"
root="$here/../../../.."
cd "$root" || exit 1
X=${XSTL_PATH:-/Users/razi/work/xStl}
mkdir -p out/scratch
c++ -DLINUX -DXSTL_UNICODE -D_UNICODE -DDEBUG -I$X/Include -IInclude -Wno-write-strings tests/arm/tools/sve_dev/irdump.cpp out/lib/libdismount_static.a $X/out/lib/libxStlcm.dylib -Wl,-rpath,$X/out/lib -o out/scratch/irdump 2>&1 | grep error
python3 tests/arm/tools/sve_dev/asmwords.py < "$2" | while read w c t; do echo "$t"; echo $w | out/scratch/irdump $1; done
