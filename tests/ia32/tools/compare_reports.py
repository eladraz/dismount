#!/usr/bin/env python3
"""
compare_reports.py BASE.json NOW.json

Compares two run_compat.py reports line by line and lists regressions: lines
whose ASM status was PASS/ALT, or whose DIS status was PASS, in BASE and are
no longer so in NOW. Exit code 1 if there are regressions, 0 otherwise.
"""
import collections
import json
import sys

ASM_OK = ('PASS', 'ALT')
DIS_OK = ('PASS',)


def load(path):
    with open(path) as f:
        report = json.load(f)
    return {(r['file'], r['line']): r for r in report['lines']}


def totals(lines, check):
    return dict(collections.Counter(r[check]['status'] for r in lines.values()))


def main():
    if len(sys.argv) != 3:
        sys.stderr.write(__doc__)
        return 2
    base = load(sys.argv[1])
    now = load(sys.argv[2])

    regressions = []
    for key, old in sorted(base.items()):
        new = now.get(key)
        for check, ok in (('asm', ASM_OK), ('dis', DIS_OK)):
            if old[check]['status'] not in ok:
                continue
            status = new[check]['status'] if new else 'MISSING'
            if status not in ok:
                error = new[check].get('error') if new else None
                regressions.append((key, check.upper(), old['source'], status, error))

    for check in ('asm', 'dis'):
        print('%s: %s -> %s' % (check.upper(), totals(base, check), totals(now, check)))
    print('regressions: %d' % len(regressions))
    for (file, line), check, source, status, error in regressions[:100]:
        print('  %s:%d %s | %s | now=%s %s' % (file, line, check, source, status, error or ''))
    return 1 if regressions else 0


if __name__ == '__main__':
    sys.exit(main())
