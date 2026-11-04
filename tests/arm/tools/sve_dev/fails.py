#!/usr/bin/env python3
"""fails.py report.json [statuses]: one line per mnemonic and status (first line of the mnemonic)"""
import json
import sys
import collections
r = json.load(open(sys.argv[1]))
want = set((sys.argv[2] if len(sys.argv) > 2 else 'FAIL,UNLIFTED,DIS_ERR,ORACLE_ERR').split(','))
seen = collections.OrderedDict()
counts = collections.Counter()
for l in r['lines']:
    if l['status'] in want:
        key = (l['mnemonic'], l['status'])
        counts[key] += 1
        if key not in seen:
            seen[key] = (l['source'], l['message'][:170])
for key, (src, msg) in seen.items():
    print('%-9s %-8s %3d | %s | %s' % (key[1], key[0], counts[key], src, msg))
