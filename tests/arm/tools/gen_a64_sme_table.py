#!/usr/bin/env python3
"""Generate Source/dismount/proc/arm/Arm64EncodingTableSme.cpp, the A64 SME
encoding table (gArm64SmeEncodingTable): SME, SME2, SME2.1 and the extensions
of them (the FP8, BFloat16, 16 bit floating point, 64 bit and lookup table
parts, the 4 way outer products and the structured sparse ones).

The groups are in a64groups_sme_*.py, the language and the emitter in
a64dsl_sme.py (which builds on a64dsl_sve.py). The order of the groups is the
order of the table: the first entry that matches wins, so every group puts its
aliases before the encoding they print for (E(..., alias=True)).

    gen_a64_sme_table.py [--check]

--check does not write the file: it samples words of every entry and of the
SME class and compares the entries with objdump (the precision and the recall
of the fixed bits), see sme_check.py.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import a64dsl_sme


def build():
    import a64groups_sme_core
    a64groups_sme_core.groups()
    for name in ('a64groups_sme_rows',):
        if os.path.exists(os.path.join(HERE, name + '.py')):
            module = __import__(name)
            module.groups()
    a64dsl_sme.order_aliases()


def main():
    build()
    if '--check' in sys.argv:
        import sme_check
        return sme_check.main([a for a in sys.argv[1:] if a != '--check'])
    a64dsl_sme.emit()
    print('%d entries -> %s' % (len(a64dsl_sme.ENTRIES), a64dsl_sme.OUTPUT))
    return 0


if __name__ == '__main__':
    sys.exit(main())
