#!/usr/bin/env python3
"""ARM compatibility driver: dismount vs. clang and objdump (see
tests/arm/TEST_PLAN.md).

  python3 tests/arm/run_compat.py [--arch a64] [--file NAME[,NAME...]]
          [--features F[,F...]] [--grep REGEX] [--strict] [--out DIR]

Per corpus line (tests/arm/corpus/<arch>/<file>.s):
  ASM   clang assembles the line, dismount assembles the line: the same bytes
        (ALT: other bytes which disassemble to the same text)
  DIS   objdump disassembles the bytes of clang, dismount disassembles them:
        the same text
  RT    dismount assembles its own text and gets the bytes of clang back
        (ALT: the same text)
  FEAT  without any feature dismount decodes the bytes only if the line is
        base, with just the features clang asks for it decodes them to the
        same text (the lines whose features dismount has no name for are
        skipped)
The lines of sve.s and sme.s are the classes which are not implemented yet:
where dismount says invalid / error they are UNSUPPORTED, not failures.

Writes OUT/report.json (the format of tests/ia32/run_compat.py, so
tests/ia32/tools/compare_reports.py reads it), OUT/summary.md and
OUT/failures.txt. Without --strict the exit status is 0 unless the harness
breaks; with --strict it is 1 on any failure.
"""
import argparse
import collections
import glob
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, 'tools'))
import armlib

UNIMPLEMENTED_FILES = ('sve', 'sme')
DEFAULT_OUT = os.path.join(armlib.REPO, 'out', 'compat-arm')


def read_corpus(path):
    """Return a list of (text, required features)."""
    lines = []
    for raw in open(path):
        raw = raw.rstrip('\n')
        if not raw.strip() or raw.startswith('//'):
            continue
        required = []
        m = re.search(r'\s*// @feature: (.*)$', raw)
        if m:
            required = m.group(1).split()
            raw = raw[:m.start()]
        lines.append((raw.strip(), required))
    return lines


def result(status, error=None):
    r = {'status': status}
    if error:
        r['error'] = error
    return r


def mattr_for(required):
    return 'none,' + ','.join('+' + f for f in required) if required else 'none'


def run_file(arch, name, lines, known_features, wanted_features):
    if arch == 't32':
        import compat_t32
        return compat_t32.run_file(name, lines, known_features, wanted_features)
    unimplemented = name in UNIMPLEMENTED_FILES
    texts = [t for t, _ in lines]
    addresses = [armlib.BASE + 4 * i for i in range(len(lines))]
    # The reference: clang with every feature
    reference = armlib.clang_assemble(texts, arch)
    ours = armlib.asm_lines(arch, texts, None, addresses)

    ref_words = []
    for status, data in reference:
        ref_words.append(armlib.hex_bytes_to_word(data) if status == 'ok' else 0)
    ref_dis = armlib.objdump_words(ref_words, arch) if ref_words else []
    our_dis = armlib.dis_words(arch, ref_words, None, addresses)

    records = []
    # The round trip: the text of dismount assembled by dismount
    rt_index = [i for i in range(len(lines)) if our_dis[i][0] == 'OK' and reference[i][0] == 'ok']
    rt_asm = armlib.asm_lines(arch, [our_dis[i][1] for i in rt_index], None,
                              [addresses[i] for i in rt_index])
    rt_result = dict(zip(rt_index, rt_asm))
    # The ALT checks: disassemble the words dismount assembled
    alt_words = []
    alt_index = []
    for i in range(len(lines)):
        if reference[i][0] == 'ok' and ours[i][0] == 'OK' and \
                armlib.hex_bytes_to_word(ours[i][1]) != ref_words[i]:
            alt_index.append(i)
            alt_words.append(armlib.hex_bytes_to_word(ours[i][1]))
    alt_texts = {}
    if alt_words:
        # Same layout as the lines: each at its own address, one by one chunks
        for i, w in zip(alt_index, alt_words):
            alt_texts[i] = w
        alt_dis = armlib.dis_words(arch, alt_words, None, [addresses[i] for i in alt_index])
        alt_texts = {i: d for i, d in zip(alt_index, alt_dis)}
    # The features
    feat_none = armlib.dis_words(arch, ref_words, 'none', addresses)
    feat_needed = {}
    by_required = collections.defaultdict(list)
    for i, (text, required) in enumerate(lines):
        if required and all(f in known_features for f in required):
            by_required[tuple(required)].append(i)
    for required, indexes in by_required.items():
        decoded = armlib.dis_words(arch, [ref_words[i] for i in indexes],
                                   mattr_for(list(required)), [addresses[i] for i in indexes])
        for i, d in zip(indexes, decoded):
            feat_needed[i] = d

    for i, (text, required) in enumerate(lines):
        record = {'file': name + '.s', 'line': i + 1, 'source': text, 'features': required}
        ref_status, ref_data = reference[i]
        if ref_status != 'ok':
            for check in ('asm', 'dis', 'rt', 'feat'):
                record[check] = result('SKIP', 'clang: ' + ref_data)
            records.append(record)
            continue
        # ASM
        status, data = ours[i]
        if status == 'OK' and data == ref_data:
            record['asm'] = result('PASS')
        elif status == 'OK':
            alt = alt_texts.get(i)
            if alt and alt[0] == 'OK' and alt[1] == ref_dis[i]:
                record['asm'] = result('ALT', 'bytes %s' % data.hex())
            else:
                record['asm'] = result('FAIL', 'got %s want %s' % (data.hex(), ref_data.hex()))
        elif unimplemented:
            record['asm'] = result('UNSUPPORTED', data)
        else:
            record['asm'] = result('FAIL', data)
        # DIS
        status, dtext = our_dis[i]
        if status == 'OK' and dtext == ref_dis[i]:
            record['dis'] = result('PASS')
        elif status in ('INVALID', 'ERR') and unimplemented:
            record['dis'] = result('UNSUPPORTED')
        elif status == 'OK':
            record['dis'] = result('FAIL', 'got "%s" want "%s"' % (dtext, ref_dis[i]))
        else:
            record['dis'] = result('FAIL', '%s want "%s"' % (status, ref_dis[i]))
        # RT
        if i in rt_result:
            rstatus, rdata = rt_result[i]
            if rstatus == 'OK' and rdata == ref_data:
                record['rt'] = result('PASS')
            elif rstatus == 'OK':
                # The other word has to disassemble to the same text
                other = armlib.dis_words(arch, [armlib.hex_bytes_to_word(rdata)], None, [addresses[i]])[0]
                if other[0] == 'OK' and other[1] == our_dis[i][1]:
                    record['rt'] = result('ALT')
                else:
                    record['rt'] = result('FAIL', 'text "%s" gives %s' % (our_dis[i][1], rdata.hex()))
            else:
                record['rt'] = result('FAIL', 'text "%s": %s' % (our_dis[i][1], rdata))
        elif unimplemented:
            record['rt'] = result('UNSUPPORTED')
        else:
            record['rt'] = result('SKIP', 'not decoded')
        # FEAT
        if unimplemented:
            record['feat'] = result('UNSUPPORTED')
        elif required and not all(f in known_features for f in required):
            record['feat'] = result('SKIP', 'no feature for ' + ' '.join(required))
        else:
            # Decoded as this instruction (and not as another one, the generic
            # msr / mrs of an encoding that the base does not have)
            none_status = feat_none[i][0]
            if none_status == 'OK' and feat_none[i][1] != ref_dis[i]:
                none_status = 'OTHER'
            if required:
                needed = feat_needed.get(i)
                if none_status == 'OK':
                    record['feat'] = result('FAIL', 'decoded without %s' % ' '.join(required))
                elif needed is None or needed[0] != 'OK' or needed[1] != ref_dis[i]:
                    record['feat'] = result('FAIL', 'not decoded with %s' % ' '.join(required))
                else:
                    record['feat'] = result('PASS')
            else:
                if none_status == 'OK' and feat_none[i][1] == ref_dis[i]:
                    record['feat'] = result('PASS')
                else:
                    record['feat'] = result('FAIL', 'base line not decoded without features')
        if wanted_features and not (set(required) & set(wanted_features)):
            pass
        records.append(record)
    return records


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--arch', default='a64', choices=['a64', 'a32', 't32', 'all'],
                        help='all: every instruction set which has a corpus')
    parser.add_argument('--file', help='corpus files (without .s), comma separated')
    parser.add_argument('--features', help='only the lines of these LLVM features')
    parser.add_argument('--grep', help='only the lines matching this regex')
    parser.add_argument('--strict', action='store_true')
    parser.add_argument('--out', default=None,
                        help='default: out/compat-arm-<arch>')
    args = parser.parse_args()
    if args.arch == 'all':
        # One run per instruction set (each with its own report)
        status = 0
        for arch in ('a64', 'a32', 't32'):
            if not glob.glob(os.path.join(HERE, 'corpus', arch, '*.s')):
                continue
            sub = [sys.executable, os.path.abspath(__file__), '--arch', arch]
            if args.out:
                sub += ['--out', args.out + '-' + arch]
            if args.strict:
                sub.append('--strict')
            if args.file:
                sub += ['--file', args.file]
            if args.features:
                sub += ['--features', args.features]
            if args.grep:
                sub += ['--grep', args.grep]
            status = max(status, subprocess.call(sub))
        return status

    if args.out is None:
        args.out = DEFAULT_OUT + '-' + args.arch
    known = set(armlib.llvm_features(args.arch))
    files = sorted(glob.glob(os.path.join(HERE, 'corpus', args.arch, '*.s')))
    wanted_files = args.file.split(',') if args.file else None
    wanted_features = args.features.split(',') if args.features else None
    grep = re.compile(args.grep) if args.grep else None

    os.makedirs(args.out, exist_ok=True)
    all_records = []
    started = time.time()
    for path in files:
        name = os.path.basename(path)[:-2]
        if wanted_files and name not in wanted_files:
            continue
        lines = read_corpus(path)
        if wanted_features:
            lines = [(t, r) for t, r in lines if set(r) & set(wanted_features)]
        if grep:
            lines = [(t, r) for t, r in lines if grep.search(t)]
        if not lines:
            continue
        records = run_file(args.arch, name, lines, known, wanted_features)
        all_records.extend(records)
        counts = collections.Counter()
        for r in records:
            for check in ('asm', 'dis', 'rt', 'feat'):
                counts[(check, r[check]['status'])] += 1
        print('%-22s %6d lines  %s' % (name, len(records), '  '.join(
            '%s %s' % (c.upper(), '/'.join('%s=%d' % (s, counts[(c, s)]) for s in
                       ('PASS', 'ALT', 'FAIL', 'UNSUPPORTED', 'SKIP') if counts[(c, s)]))
            for c in ('asm', 'dis', 'rt', 'feat'))))
        sys.stdout.flush()

    with open(os.path.join(args.out, 'report.json'), 'w') as f:
        json.dump({'arch': args.arch, 'lines': all_records}, f)
    failures = [r for r in all_records
                if any(r[c]['status'] == 'FAIL' for c in ('asm', 'dis', 'rt', 'feat'))]
    with open(os.path.join(args.out, 'failures.txt'), 'w') as f:
        for r in failures:
            for c in ('asm', 'dis', 'rt', 'feat'):
                if r[c]['status'] == 'FAIL':
                    f.write('%s:%d %s | %s | %s\n' % (r['file'], r['line'], c.upper(),
                                                       r['source'], r[c].get('error', '')))
    totals = collections.Counter()
    per_file = collections.defaultdict(collections.Counter)
    for r in all_records:
        for c in ('asm', 'dis', 'rt', 'feat'):
            totals[(c, r[c]['status'])] += 1
            per_file[r['file']][(c, r[c]['status'])] += 1
    with open(os.path.join(args.out, 'summary.md'), 'w') as f:
        f.write('# ARM compatibility (%s)\n\n%d lines, %d with a failure, %.0f s\n\n' % (
            args.arch, len(all_records), len(failures), time.time() - started))
        f.write('| file | lines | ASM fail | DIS fail | RT fail | FEAT fail |\n|---|---|---|---|---|---|\n')
        for name, c in sorted(per_file.items()):
            n = sum(v for (chk, s), v in c.items() if chk == 'asm')
            f.write('| %s | %d | %d | %d | %d | %d |\n' % (
                name, n, c[('asm', 'FAIL')], c[('dis', 'FAIL')], c[('rt', 'FAIL')], c[('feat', 'FAIL')]))
    print('\n%d lines, %d with a failure (see %s)' % (len(all_records), len(failures), args.out))
    for c in ('asm', 'dis', 'rt', 'feat'):
        print('  %-5s %s' % (c.upper(), dict((s, totals[(c, s)]) for s in
                                            ('PASS', 'ALT', 'FAIL', 'UNSUPPORTED', 'SKIP') if totals[(c, s)])))
    return 1 if (args.strict and failures) else 0


if __name__ == '__main__':
    sys.exit(main())
