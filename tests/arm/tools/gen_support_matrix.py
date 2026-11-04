#!/usr/bin/env python3
"""Write tests/arm/SUPPORT.md, the support matrix of the ARM instruction sets
(tests/arm/TEST_PLAN.md): for every feature what dismount does
(ArmFeatures::getSupport(), read with "dismount_cli features"), how many
encodings the tables have and how many corpus lines of the feature pass in
the latest run of tests/arm/run_compat.py.

It also checks the marks: a feature marked SUPPORT_DISASSEMBLE / ASSEMBLE /
ENCODING_VERIFIED has encodings, and no line of its corpus files fails in the
report; a feature marked SUPPORT_LIFT has no UNLIFTED line in the semantics
report (when there is one, out/semantics-arm/report.json). Exit status 1 on
an inconsistency.

    gen_support_matrix.py [--report out/compat-arm/report.json]
"""
import argparse
import collections
import glob
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import armlib

OUTPUT = os.path.join(armlib.REPO, 'tests', 'arm', 'SUPPORT.md')

SUPPORT_DISASSEMBLE = 0x01
SUPPORT_ASSEMBLE = 0x02
SUPPORT_LIFT = 0x04
SUPPORT_PARTIAL_LIFT = 0x08
SUPPORT_LIFT_VERIFIED = 0x10
SUPPORT_ENCODING_VERIFIED = 0x20


def mark(support, bit):
    return 'yes' if support & bit else '-'


_ENTRIES = {}
# The first byte of the 32 bit Thumb encodings of Advanced SIMD and the A32 byte they translate to
_T32_SIMD_TO_A32 = {0xEF: 0xF2, 0xFF: 0xF3, 0xF9: 0xF4}


def aarch32_entries(arch):
    if arch not in _ENTRIES:
        _ENTRIES[arch] = armlib.entries(arch)
    return _ENTRIES[arch]


def aarch32_features(arch, hexbytes):
    """The feature indexes of the encodings of the instructions in the bytes of a corpus line."""
    code = bytes.fromhex(hexbytes)
    found = set()
    position = 0
    while position < len(code):
        if arch == 'a32':
            size = 4
            word = int.from_bytes(code[position:position + 4], 'little')
            candidates = [word]
        else:
            first = int.from_bytes(code[position:position + 2], 'little')
            if (first >> 11) in (0x1D, 0x1E, 0x1F):
                size = 4
                word = (first << 16) | int.from_bytes(code[position + 2:position + 4], 'little')
                top = word >> 24
                translated = ((_T32_SIMD_TO_A32.get(top, top)) << 24) | (word & 0xFFFFFF)
                candidates = [word, translated]
            else:
                size = 2
                word = first
                candidates = [word]
        position += size
        for candidate in candidates:
            hit = None
            for mask, value, feature, flags, esize, mnemonic in aarch32_entries(arch):
                if esize == size and (candidate & mask) == value:
                    hit = feature
                    break
            if hit is None and arch == 't32' and size == 4 and candidate == candidates[-1]:
                for mask, value, feature, flags, esize, mnemonic in aarch32_entries('a32'):
                    if (candidate & mask) == value:
                        hit = feature
                        break
            if hit is not None:
                found.add(hit)
                break
    return found


def count_aarch32_line(r, counters):
    """Count a line of an AArch32 semantics report by the features of its instructions."""
    status = r.get('status')
    if status in ('SKIP', 'ASM_ERR') or not r.get('hex'):
        return
    arch = r['arch']
    names = dict((index, name) for index, name, isa, support, entries in armlib.features(arch))
    for feature in aarch32_features(arch, r['hex']):
        key = '' if names.get(feature, '-') == '-' else names[feature]
        counters[(arch, key)][status] += 1
        counters[(arch, key)]['total'] += 1


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--report', default=os.path.join(armlib.REPO, 'out', 'compat-arm', 'report.json'))
    parser.add_argument('--semantics', nargs='*', default=[
        os.path.join(armlib.REPO, 'out', 'semantics_arm', 'report.json'),
        os.path.join(armlib.REPO, 'out', 'semantics_arm_host', 'report.json'),
        os.path.join(armlib.REPO, 'out', 'semantics_arm_a32', 'report.json'),
        os.path.join(armlib.REPO, 'out', 'semantics_arm_t32', 'report.json')])
    args = parser.parse_args()

    # The reports of run_compat.py of each instruction set: --report (a64 by
    # default) and the out/compat-arm*/report.json files of the other sets
    lines_of = {}
    failed = {}
    reports = {}
    paths = [args.report] + sorted(glob.glob(os.path.join(armlib.REPO, 'out', 'compat-arm-*', 'report.json')))
    for path in paths:
        if not os.path.exists(path):
            continue
        report = json.load(open(path))
        reports[report.get('arch', 'a64')] = report
    for arch, report in reports.items():
        lines_of[arch] = collections.defaultdict(collections.Counter)
        failed[arch] = collections.Counter()
        for r in report['lines']:
            names = r['features'] or ['']
            for name in names:
                ok = all(r[c]['status'] in ('PASS', 'ALT') for c in ('asm', 'dis', 'rt'))
                lines_of[arch][name]['total'] += 1
                lines_of[arch][name]['pass'] += 1 if ok else 0
                if any(r[c]['status'] == 'FAIL' for c in ('asm', 'dis', 'rt', 'feat')):
                    failed[arch][name] += 1
    # The semantics reports: the A64 lines by the feature of their tag or of their mnemonic, the
    # AArch32 ones by the features of the encodings of their instructions (sem32[(arch, feature name)])
    sem32 = collections.defaultdict(collections.Counter)
    unlifted = collections.Counter()
    sem_failed = collections.Counter()
    sem_pass = collections.Counter()
    sem_total = collections.Counter()
    reports = [path for path in args.semantics if os.path.exists(path)]
    if reports:
        by_mnemonic = collections.defaultdict(set)
        names = dict((index, name) for index, name, isa, support, entries in armlib.features('a64'))
        for mask, value, feature, flags, size, mnemonic in armlib.entries('a64'):
            by_mnemonic[mnemonic].add('' if names.get(feature, '-') == '-' else names[feature])
        for path in reports:
            for r in json.load(open(path)).get('lines', []):
                if r.get('arch', 'a64') != 'a64':
                    count_aarch32_line(r, sem32)
                    continue
                tag = r.get('feature', '')
                keys = [tag] if tag else sorted(by_mnemonic.get(r.get('mnemonic', '').split('.')[0], ['']))
                for key in keys:
                    status = r.get('status')
                    if status == 'UNLIFTED':
                        unlifted[key] += 1
                    elif status == 'FAIL':
                        sem_failed[key] += 1
                    elif status == 'PASS':
                        sem_pass[key] += 1
                    if status not in ('SKIP', 'ASM_ERR'):
                        sem_total[key] += 1

    # An AArch32 feature can have its encodings in one of the two views only
    # (hwdiv: T32, hwdiv-arm: A32)
    entries_any = collections.Counter()
    for view in ('a32', 't32'):
        for index, name, isa, support, entries in armlib.features(view):
            entries_any[index] = max(entries_any[index], entries)

    problems = []
    out = ['# ARM support matrix\n',
           '\nGenerated by `tests/arm/tools/gen_support_matrix.py` from `dismount_cli features`',
           '(`ArmFeatures::getSupport()`), the latest `tests/arm/run_compat.py` report and the',
           'semantics reports of `tests/arm/semantics/run_semantics.py` (the oracles: Unicorn and the M4).',
           '"lift" is every instruction of the feature, "partial" some of them (the comment of the',
           'feature in `ArmFeatures.cpp` says which); "lift verified" means the lifted instructions',
           'pass the differential tests against a processor, otherwise they are checked by',
           '`tests/TestArm64Semantics.cpp` only.',
           'A feature is marked with what dismount does for *every* instruction of it; the',
           'encodings count is the number of entries of the encoding tables with the feature.',
           'The system registers and the operations of `sys` are not gated by a feature (like',
           'LLVM names them), so features that only add such names have no encodings.\n']
    for arch, title in (('a64', 'A64'), ('a32', 'A32'), ('t32', 'T32')):
        rows = armlib.features(arch)
        out.append('\n## %s\n' % title)
        out.append('| id | LLVM feature | encodings | disassemble | assemble | lift | lift verified '
                   '| encoding verified | corpus lines passing | semantics lines passing |')
        out.append('|---|---|---|---|---|---|---|---|---|---|')
        for index, name, isa, support, entries in rows:
            shown = name if name != '-' else '(base)'
            key = '' if name == '-' else name
            counts = lines_of.get(arch, {}).get(key, collections.Counter())
            corpus = '%d / %d' % (counts['pass'], counts['total']) if counts['total'] else '-'
            lift = 'yes' if support & SUPPORT_LIFT else ('partial' if support & SUPPORT_PARTIAL_LIFT else '-')
            if arch == 'a64':
                semantics = '%d / %d' % (sem_pass.get(key, 0), sem_total.get(key, 0)) \
                    if sem_total.get(key) else '-'
            else:
                counts32 = sem32.get((arch, key), collections.Counter())
                semantics = '%d / %d' % (counts32['PASS'] + counts32['ORACLE_BUG'], counts32['total']) \
                    if counts32['total'] else '-'
            out.append('| %d | %s | %d | %s | %s | %s | %s | %s | %s | %s |' % (
                index, shown, entries, mark(support, SUPPORT_DISASSEMBLE),
                mark(support, SUPPORT_ASSEMBLE), lift, mark(support, SUPPORT_LIFT_VERIFIED),
                mark(support, SUPPORT_ENCODING_VERIFIED), corpus, semantics))
            if support & (SUPPORT_DISASSEMBLE | SUPPORT_ASSEMBLE | SUPPORT_ENCODING_VERIFIED):
                if entries == 0 and (arch == 'a64' or entries_any[index] == 0):
                    problems.append('%s is marked but has no encodings' % shown)
                if failed.get(arch, {}).get(key):
                    problems.append('%s (%s) is marked and %d corpus lines fail' % (
                        shown, arch, failed[arch][key]))
            if support & SUPPORT_LIFT and unlifted.get(key) and arch == 'a64':
                problems.append('%s is marked SUPPORT_LIFT and has %d unlifted lines' % (shown, unlifted[key]))
            if arch != 'a64' and reports:
                counts32 = sem32.get((arch, key), collections.Counter())
                if support & SUPPORT_LIFT and counts32['UNLIFTED']:
                    problems.append('%s (%s) is marked SUPPORT_LIFT and has %d unlifted semantics lines' %
                                    (shown, arch, counts32['UNLIFTED']))
                if support & SUPPORT_LIFT_VERIFIED:
                    if counts32['FAIL']:
                        problems.append('%s (%s) is marked SUPPORT_LIFT_VERIFIED and %d semantics lines fail' %
                                        (shown, arch, counts32['FAIL']))
                    if not counts32['PASS'] and not (sem32.get(('a32' if arch == 't32' else 't32', key)) or {}):
                        problems.append('%s (%s) is marked SUPPORT_LIFT_VERIFIED and no semantics line passes' %
                                        (shown, arch))
            if support & SUPPORT_LIFT_VERIFIED and reports and arch == 'a64':
                if sem_failed.get(key):
                    problems.append('%s is marked SUPPORT_LIFT_VERIFIED and %d semantics lines fail' %
                                    (shown, sem_failed[key]))
                if not sem_pass.get(key):
                    problems.append('%s is marked SUPPORT_LIFT_VERIFIED and no semantics line passes' % shown)
    with open(OUTPUT, 'w') as f:
        f.write('\n'.join(out) + '\n')
    print('wrote %s' % OUTPUT)
    for p in problems:
        print('PROBLEM: ' + p)
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
