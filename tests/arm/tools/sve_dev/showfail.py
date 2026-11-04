#!/usr/bin/env python3
"""showfail.py report.json 'source substring' [n]: the first n failing states of the line, only the registers named in the line and the diffs"""
import json
import re
import sys
r = json.load(open(sys.argv[1]))
sub = sys.argv[2]
n = int(sys.argv[3]) if len(sys.argv) > 3 else 2
for l in r['lines']:
    if l['status'] == 'FAIL' and sub in l['source']:
        print('==', l['source'], l['hex'])
        names = set(re.findall(r'\b([zpx]\d+|v\d+)\b', l['source'])) | {'fpcr', 'fpsr', 'nzcv', 'ffr'}
        for d in l['diffs'][:n]:
            toks = d['state'].split(';', 1)[1].split()
            print(' in :', ' '.join(t for t in toks if t.split('=')[0] in names))
            print(' dif:', d['diff'][:4])
        break
