#!/usr/bin/env python3
"""Generate Source/dismount/proc/arm/Arm64EncodingTable.cpp, the A64 base
encoding table (gArm64BaseEncodingTable).

The groups are in a64groups_*.py, the language and the emitter in a64dsl.py.
The order of the groups is the order of the table: the first entry that
matches wins, so every group puts its aliases before the encoding they print
for.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import a64dsl
import a64groups_dp
import a64groups_system
import a64groups_loadstore

REPO = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
OUTPUT = os.path.join(REPO, 'Source', 'dismount', 'proc', 'arm', 'Arm64EncodingTable.cpp')

HELPERS = r'''// True if the sys alias of 'scheme' names the operation of the instruction and
// takes (or not) a register
bool isNamedOption(uint32 e, uint scheme, bool takesRegister)
{
    uint value = BITS(18, 5);
    return (getOptionName(scheme, value) != NULL) &&
           (optionTakesRegister(scheme, value) == takesRegister);
}

// True if 'value' (of a register of 'size' bits) is a movz of one 16 bit chunk
bool isMovzValue(uint64 value, uint size)
{
    if (size == 32)
        value &= 0xFFFFFFFFu;
    for (uint shift = 0; shift < size; shift += 16)
        if ((value & ~((uint64)0xffff << shift)) == 0)
            return true;
    return false;
}

// True if 'value' is built by a movz or a movn
bool isMoveWideValue(uint64 value, uint size)
{
    uint64 inverted = ~value;
    if (size == 32)
        inverted &= 0xFFFFFFFFu;
    return isMovzValue(value, size) || isMovzValue(inverted, size);
}

// The mov alias of an orr (bitmask immediate): the value is not a move wide
bool isLogicalMoveAlias(uint32 e, uint size)
{
    uint64 value;
    if (!decodeLogicalImmediateValue(BITS(22, 22), BITS(21, 16), BITS(15, 10), size, value))
        return false;
    return !isMoveWideValue(value, size);
}

// The mov alias of a movn: the value is not a movz (lsl #0 takes precedence)
bool isMoveNegatedAlias(uint32 e, uint size)
{
    uint hw = BITS(22, 21);
    uint64 imm16 = BITS(20, 5);
    if ((imm16 == 0) && (hw != 0))
        return false;
    uint64 value = ~(imm16 << (hw * 16));
    if (size == 32)
        value &= 0xFFFFFFFFu;
    return !isMovzValue(value, size);
}
'''


def main():
    a64groups_dp.dp_immediate()
    a64groups_dp.dp_register()
    a64groups_dp.branches()
    a64groups_dp.exceptions()
    a64groups_system.system()
    a64groups_loadstore.loadstore()
    a64dsl.finalize_conditions()
    a64dsl.emit(HELPERS, OUTPUT)
    print('%d entries -> %s' % (len(a64dsl.ENTRIES), OUTPUT))


if __name__ == '__main__':
    main()
