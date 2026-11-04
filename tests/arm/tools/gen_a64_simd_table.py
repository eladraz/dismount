#!/usr/bin/env python3
"""Generate Source/dismount/proc/arm/Arm64EncodingTableSimd.cpp, the A64
scalar floating point, Advanced SIMD and cryptographic encoding table
(gArm64SimdEncodingTable).

The groups are in a64groups_fp.py, a64groups_simd.py, a64groups_crypto.py and
a64groups_simdmem.py, the language in a64dsl.py and a64simd_dsl.py. The order
of the groups is the order of the table: the first entry that matches wins, so
every group puts its aliases before the encoding they print for.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import a64dsl
import a64simd_dsl
import a64groups_fp
import a64groups_simd
import a64groups_crypto
import a64groups_simdmem

REPO = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
OUTPUT = os.path.join(REPO, 'Source', 'dismount', 'proc', 'arm', 'Arm64EncodingTableSimd.cpp')

HELPERS = r'''// The conditions of the entries: the constraints between the fields which a
// mask can't express'''


def main():
    a64groups_fp.scalar_fp()
    a64groups_simd.simd_vector_all()
    a64groups_crypto.crypto()
    a64groups_simdmem.simd_memory()
    a64dsl.finalize_conditions()
    a64simd_dsl.emit(HELPERS, OUTPUT)
    print('%d entries -> %s' % (len(a64dsl.ENTRIES), OUTPUT))


if __name__ == '__main__':
    main()
