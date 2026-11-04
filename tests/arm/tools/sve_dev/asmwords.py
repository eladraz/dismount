#!/usr/bin/env python3
"""asmwords.py: assemble the instruction lines of stdin (clang, tests/arm/semantics/armasm.py) and print
`<word hex> # <text>`; the debugging scripts of this directory pipe the words to opdump / irdump."""
import os
import sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'semantics'))
import armasm
lines = [l.strip() for l in sys.stdin if l.strip()]
res = armasm.assemble('a64', lines)
for t, (h, e) in zip(lines, res):
    if e:
        print('ERR', t, e, file=sys.stderr)
        continue
    b = bytes.fromhex(h)
    print('%08x' % int.from_bytes(b[:4], 'little'), '#', t)
