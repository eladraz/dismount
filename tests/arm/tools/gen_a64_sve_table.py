#!/usr/bin/env python3
"""Generate Source/dismount/proc/arm/Arm64EncodingTableSve.cpp, the A64 SVE
encoding table (gArm64SveEncodingTable): SVE, SVE2, SVE2.1 and the instructions
of the SME2 extension which are in the SVE encoding space.

The groups are in a64groups_sve_*.py, the language and the emitter in
a64dsl_sve.py. The order of the groups is the order of the table: the first
entry that matches wins, so every group puts its aliases before the encoding
they print for (E(..., alias=True) does it: the aliases of all the groups come
first).

    gen_a64_sve_table.py [--check]

--check does not write the file: it samples words of every entry and of the
SVE class and compares the entries with objdump (the precision and the recall
of the fixed bits), see sve_check.py.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import a64dsl_sve

REPO = a64dsl_sve.REPO
OUTPUT = os.path.join(REPO, 'Source', 'dismount', 'proc', 'arm', 'Arm64EncodingTableSve.cpp')

HELPERS = r'''// True if the logical immediate field (N:immr:imms, bits 5-17) of dupm / and /
// orr / eor is valid
bool isValidLogicalImmediate(uint32 e)
{
    uint64 value;
    return decodeLogicalImmediateValue(BITS(17, 17), BITS(16, 11), BITS(10, 5), 64, value);
}

// True if a value of 'bits' bit elements can be made by dup / cpy: an 8 bit
// signed number, shifted left by 8 or not (not for the byte elements)
bool isCpyDupImmediate(uint bits, uint64 value)
{
    int64 v = (bits == 64) ? (int64)value :
              signExtend(value & (((uint64)1 << bits) - 1), bits);
    if ((v >= -128) && (v <= 127))
        return true;
    return (bits > 8) && ((v & 0xff) == 0) && ((v >> 8) >= -128) && ((v >> 8) <= 127);
}

// True if the 64 bit value is made of identical elements of 'bits' bits
bool isIdenticalElements(uint bits, uint64 value)
{
    uint64 mask = ((uint64)1 << bits) - 1;
    for (uint i = bits; i < 64; i += bits)
        if (((value >> i) & mask) != (value & mask))
            return false;
    return true;
}

// The mov alias of dupm is printed when dup can't make the immediate
bool isDupmMoveAlias(uint32 e)
{
    uint64 value;
    if (!decodeLogicalImmediateValue(BITS(17, 17), BITS(16, 11), BITS(10, 5), 64, value))
        return false;
    if (isCpyDupImmediate(64, value))
        return false;
    for (uint bits = 32; bits >= 8; bits /= 2)
        if (isIdenticalElements(bits, value) && isCpyDupImmediate(bits, value))
            return false;
    return true;
}
'''


def build():
    import a64groups_sve_int
    import a64groups_sve_pred
    import a64groups_sve_fp
    import a64groups_sve_mem
    import a64groups_sve_sve2
    import a64groups_sve_new
    a64groups_sve_int.groups()
    a64groups_sve_pred.groups()
    a64groups_sve_fp.groups()
    a64groups_sve_mem.groups()
    a64groups_sve_sve2.groups()
    a64groups_sve_new.groups()
    a64dsl_sve.order_aliases()


def main():
    build()
    if '--check' in sys.argv:
        import sve_check
        return sve_check.main([a for a in sys.argv[1:] if a != '--check'])
    a64dsl_sve.emit(HELPERS, OUTPUT)
    print('%d entries -> %s' % (len(a64dsl_sve.ENTRIES), OUTPUT))
    return 0


if __name__ == '__main__':
    sys.exit(main())
