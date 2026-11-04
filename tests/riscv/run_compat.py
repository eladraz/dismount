#!/usr/bin/env python3
"""RISC-V compatibility driver: dismount vs. llvm-mc and llvm-objdump (see
tests/riscv/TEST_PLAN.md).

  python3 tests/riscv/run_compat.py [--arch rv64|rv32|all] [--file NAME[,NAME...]]
          [--features F[,F...]] [--grep REGEX] [--strict] [--out DIR]

Per corpus line (tests/riscv/corpus/<arch>/<file>.s, "text  # @feature: names"):
  ASM   llvm-mc assembles the line without compression, dismount assembles the
        line (--no-rvc): the same bytes. A line which LLVM rejects must be
        rejected by dismount. (ALT: other bytes which disassemble to the same text)
  ASMC  the same with the compression on (the lines which are not c.*)
  DIS   llvm-objdump disassembles the bytes of llvm-mc, dismount disassembles
        them: the same text (default format)
  DISNA the same with -M no-aliases / --format no-aliases
  DISNUM the same with -M numeric / --format numeric
  RT    dismount assembles its own text (the default text of the 32 bit
        instructions and the no-aliases text of all of them) and gets the
        bytes of llvm-mc back (ALT: the same text)
  FEAT  with the features the line needs and without the primary one, llvm
        and dismount agree on the text of the bytes (or that they are
        invalid) and on assembling the line (or rejecting it)
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
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, 'tools'))
import rvlib

DEFAULT_OUT = os.path.join(rvlib.REPO, 'out', 'compat-riscv')
CHECKS = ('asm', 'asmc', 'dis', 'disna', 'disnum', 'rt', 'feat')
BRANCHES = set('beq bne blt bge bltu bgeu beqz bnez blez bgez bltz bgtz j jal c.j c.jal c.beqz c.bnez'.split())


def read_corpus(path):
    """Return a list of (text, required features)."""
    lines = []
    for raw in open(path):
        raw = raw.rstrip('\n')
        if not raw.strip() or raw.startswith('#'):
            continue
        required = []
        m = re.search(r'\s*# @feature: (.*)$', raw)
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


def relative_text(text, address, bits=64):
    """The text of a disassembly with the absolute branch target as the
    offset from the instruction (the form the assembler reads)."""
    words = text.split(' ', 1)
    if words[0] not in BRANCHES or len(words) < 2:
        return text
    parts = words[1].split(', ')
    m = re.fullmatch(r'0x([0-9a-f]+)', parts[-1])
    if not m:
        return text
    offset = int(m.group(1), 16) - address
    if bits == 32:
        offset = (offset + (1 << 31)) % (1 << 32) - (1 << 31)
    parts[-1] = str(offset)
    return words[0] + ' ' + ', '.join(parts)


def mattr_none(bits, names):
    return 'none,' + ','.join('+' + n for n in names) if names else 'none'


def run_file(bits, name, lines, wanted_features):
    texts = [t for t, _ in lines]
    is_c = [t.startswith('c.') for t in texts]
    all_features = rvlib.all_features(bits)
    n = len(lines)
    addresses = [rvlib.BASE] * n

    # The reference: llvm-mc, the explicit c.* lines in their own run
    reference = [None] * n
    for compressed in (False, True):
        index = [i for i in range(n) if is_c[i] == compressed]
        if not index:
            continue
        got = rvlib.llvm_assemble([texts[i] for i in index], bits, all_features, rvc=compressed)
        for i, r in zip(index, got):
            reference[i] = r
    # With the compression (the 32 bit lines)
    index_c = [i for i in range(n) if not is_c[i]]
    reference_c = {}
    if index_c:
        got = rvlib.llvm_assemble([texts[i] for i in index_c], bits, all_features, rvc=True)
        reference_c = dict(zip(index_c, got))

    ours = rvlib.dismount_asm(texts, bits, None, rvc=False)
    ours_c = rvlib.dismount_asm(texts, bits, None, rvc=True)

    # The units to disassemble: the lines which are one instruction
    units = []
    unit_of = {}
    for i in range(n):
        status, data = reference[i]
        if status == 'ok' and len(data) in (2, 4):
            unit_of[i] = len(units)
            units.append((rvlib.BASE + 4 * len(units), data))
    # (the units are placed contiguous for objdump: it reads them in sequence
    # so the addresses are the running offsets)
    running = []
    position = rvlib.BASE
    for _, d in units:
        running.append((position, d))
        position += len(d)
    units = running

    def disassemble(fmt_dismount, fmt_objdump):
        theirs = rvlib.objdump_units(units, bits, all_features, fmt_objdump)
        mine = rvlib.dismount_dis(units, bits, None, fmt_dismount)
        return theirs, mine

    default_t, default_m = disassemble(None, [])
    na_t, na_m = disassemble('no-aliases', ['-M', 'no-aliases'])
    num_t, num_m = disassemble('numeric', ['-M', 'numeric'])

    # RT: dismount's texts assembled by dismount
    rt_inputs = []
    rt_index = []
    for i in range(n):
        if i not in unit_of:
            continue
        u = unit_of[i]
        address = units[u][0]
        for kind, source in (('default', default_m[u]), ('noalias', na_m[u])):
            if source[0] != 'OK':
                continue
            if kind == 'default' and is_c[i]:
                continue
            rt_inputs.append(relative_text(source[1], address, bits))
            rt_index.append((i, kind))
    rt_out = rvlib.dismount_asm(rt_inputs, bits, None, rvc=False) if rt_inputs else []
    rt_map = collections.defaultdict(list)
    for (i, kind), text, out in zip(rt_index, rt_inputs, rt_out):
        rt_map[i].append((kind, text, out))

    # FEAT
    feat_state = {}
    groups = collections.defaultdict(list)
    for i, (text, required) in enumerate(lines):
        if required and reference[i][0] == 'ok' and i in unit_of:
            groups[tuple(required)].append(i)
    for required, indexes in groups.items():
        on = rvlib.feature_closure(bits, mattr_none(bits, list(required)))
        off_spec = mattr_none(bits, list(required)) + ',-' + required[0]
        off = rvlib.feature_closure(bits, off_spec)
        sub = [units[unit_of[i]] for i in indexes]
        sub_units = []
        position = rvlib.BASE
        for _, d in sub:
            sub_units.append((position, d))
            position += len(d)
        for label, features, spec in (('on', on, mattr_none(bits, list(required))), ('off', off, off_spec)):
            theirs = rvlib.objdump_units(sub_units, bits, features)
            mine = rvlib.dismount_dis(sub_units, bits, spec)
            # assembling
            sub_texts = [texts[i] for i in indexes]
            c_lines = [t for t in sub_texts if t.startswith('c.')]
            asm_mc = []
            for compressed in (False, True):
                part = [(k, t) for k, t in enumerate(sub_texts) if t.startswith('c.') == compressed]
                if not part:
                    continue
                got = rvlib.llvm_assemble([t for _, t in part], bits, features, rvc=compressed)
                asm_mc.extend((k, g) for (k, _), g in zip(part, got))
            asm_mc = dict(asm_mc)
            asm_dm = rvlib.dismount_asm(sub_texts, bits, spec, rvc=False)
            for k, i in enumerate(indexes):
                problems = []
                t = theirs[k]
                m = mine[k]
                got_text = m[1] if m[0] == 'OK' else '<unknown>'
                if t != got_text:
                    problems.append('%s decode: llvm "%s" dismount "%s"' % (label, t, got_text))
                a = asm_mc[k]
                d = asm_dm[k]
                if (a[0] == 'ok') != (d[0] == 'OK'):
                    problems.append('%s assemble: llvm %s dismount %s' % (
                        label, 'accepts' if a[0] == 'ok' else 'rejects',
                        'accepts' if d[0] == 'OK' else 'rejects'))
                elif a[0] == 'ok' and a[1] != d[1]:
                    problems.append('%s assemble: llvm %s dismount %s' % (label, a[1].hex(), d[1].hex()))
                feat_state.setdefault(i, []).extend(problems)

    records = []
    for i, (text, required) in enumerate(lines):
        record = {'file': name + '.s', 'line': i + 1, 'source': text, 'features': required}
        ref_status, ref_data = reference[i]
        # ASM
        status, data = ours[i]
        if ref_status != 'ok':
            record['asm'] = result('PASS') if status == 'ERR' else result(
                'FAIL', 'llvm rejects, dismount gives %s' % data.hex())
        elif status == 'OK' and data == ref_data:
            record['asm'] = result('PASS')
        elif status == 'OK':
            record['asm'] = result('FAIL', 'got %s want %s' % (data.hex(), ref_data.hex()))
        else:
            record['asm'] = result('FAIL', data)
        # ASMC
        if is_c[i]:
            record['asmc'] = result('SKIP', 'explicit c.*')
        else:
            rs, rd = reference_c[i]
            status, data = ours_c[i]
            if rs != 'ok':
                record['asmc'] = result('PASS') if status == 'ERR' else result('FAIL', 'llvm rejects')
            elif status == 'OK' and data == rd:
                record['asmc'] = result('PASS')
            elif status == 'OK':
                record['asmc'] = result('FAIL', 'got %s want %s' % (data.hex(), rd.hex()))
            else:
                record['asmc'] = result('FAIL', data)
        # DIS
        for key, theirs, mine in (('dis', default_t, default_m), ('disna', na_t, na_m),
                                  ('disnum', num_t, num_m)):
            if i not in unit_of:
                record[key] = result('SKIP', 'not one instruction')
                continue
            u = unit_of[i]
            m = mine[u]
            got = m[1] if m[0] == 'OK' else '<unknown>'
            if got == theirs[u]:
                record[key] = result('PASS')
            else:
                record[key] = result('FAIL', 'got "%s" want "%s"' % (got, theirs[u]))
        # RT
        if i not in unit_of or not rt_map.get(i):
            record['rt'] = result('SKIP', 'no text')
        else:
            problems = []
            for kind, text, out in rt_map[i]:
                if out[0] == 'OK' and out[1] == ref_data:
                    continue
                if out[0] == 'OK':
                    # the other bytes have to disassemble to the same text
                    other = rvlib.dismount_dis([(rvlib.BASE, out[1])], bits)[0]
                    if other[0] == 'OK' and other[1] == default_m[unit_of[i]][1]:
                        continue
                    problems.append('%s text "%s" gives %s' % (kind, text, out[1].hex()))
                else:
                    problems.append('%s text "%s": %s' % (kind, text, out[1]))
            record['rt'] = result('FAIL', '; '.join(problems)) if problems else result('PASS')
        # FEAT
        if not required or i not in feat_state and ref_status == 'ok' and i in unit_of and not required:
            record['feat'] = result('SKIP', 'no feature')
        elif ref_status != 'ok' or i not in unit_of:
            record['feat'] = result('SKIP', 'llvm rejects or not one instruction')
        elif feat_state.get(i):
            record['feat'] = result('FAIL', '; '.join(feat_state[i][:3]))
        else:
            record['feat'] = result('PASS')
        records.append(record)
    return records


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--arch', default='all', choices=['rv32', 'rv64', 'all'])
    parser.add_argument('--file', help='corpus files (without .s), comma separated')
    parser.add_argument('--features', help='only the lines of these LLVM features')
    parser.add_argument('--grep', help='only the lines matching this regex')
    parser.add_argument('--strict', action='store_true')
    parser.add_argument('--out', default=None, help='default: out/compat-riscv-<arch>')
    args = parser.parse_args()
    if args.arch == 'all':
        status = 0
        import subprocess
        for arch in ('rv64', 'rv32'):
            sub = [sys.executable, os.path.abspath(__file__), '--arch', arch]
            if args.out:
                sub += ['--out', args.out + '-' + arch]
            for flag in ('file', 'features', 'grep'):
                if getattr(args, flag):
                    sub += ['--' + flag, getattr(args, flag)]
            if args.strict:
                sub.append('--strict')
            status = max(status, subprocess.call(sub))
        return status

    bits = int(args.arch[2:])
    out_dir = args.out or (DEFAULT_OUT + '-' + args.arch)
    files = sorted(glob.glob(os.path.join(HERE, 'corpus', args.arch, '*.s')))
    wanted_files = args.file.split(',') if args.file else None
    wanted_features = args.features.split(',') if args.features else None
    grep = re.compile(args.grep) if args.grep else None

    os.makedirs(out_dir, exist_ok=True)
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
        records = run_file(bits, name, lines, wanted_features)
        all_records.extend(records)
        counts = collections.Counter()
        for r in records:
            for check in CHECKS:
                counts[(check, r[check]['status'])] += 1
        print('%-22s %6d lines  %s' % (name, len(records), '  '.join(
            '%s %s' % (c.upper(), '/'.join('%s=%d' % (s, counts[(c, s)]) for s in
                       ('PASS', 'ALT', 'FAIL', 'SKIP') if counts[(c, s)]))
            for c in CHECKS if counts[(c, 'FAIL')])))
        sys.stdout.flush()

    with open(os.path.join(out_dir, 'report.json'), 'w') as f:
        json.dump({'arch': args.arch, 'lines': all_records}, f)
    failures = [r for r in all_records if any(r[c]['status'] == 'FAIL' for c in CHECKS)]
    with open(os.path.join(out_dir, 'failures.txt'), 'w') as f:
        for r in failures:
            for c in CHECKS:
                if r[c]['status'] == 'FAIL':
                    f.write('%s:%d %s | %s | %s\n' % (r['file'], r['line'], c.upper(),
                                                       r['source'], r[c].get('error', '')))
    totals = collections.Counter()
    per_file = collections.defaultdict(collections.Counter)
    for r in all_records:
        for c in CHECKS:
            totals[(c, r[c]['status'])] += 1
            per_file[r['file']][(c, r[c]['status'])] += 1
    with open(os.path.join(out_dir, 'summary.md'), 'w') as f:
        f.write('# RISC-V compatibility (%s)\n\n%d lines, %d with a failure, %.0f s\n\n' % (
            args.arch, len(all_records), len(failures), time.time() - started))
        f.write('| file | lines | ' + ' | '.join(c.upper() + ' fail' for c in CHECKS) + ' |\n')
        f.write('|---|---|' + '---|' * len(CHECKS) + '\n')
        for name, c in sorted(per_file.items()):
            n = sum(v for (chk, s), v in c.items() if chk == 'asm')
            f.write('| %s | %d | %s |\n' % (name, n, ' | '.join(str(c[(chk, 'FAIL')]) for chk in CHECKS)))
    print('\n%d lines, %d with a failure (see %s)' % (len(all_records), len(failures), out_dir))
    for c in CHECKS:
        print('  %-6s %s' % (c.upper(), dict((s, totals[(c, s)]) for s in
                                           ('PASS', 'ALT', 'FAIL', 'SKIP') if totals[(c, s)])))
    return 1 if (args.strict and failures) else 0


if __name__ == '__main__':
    sys.exit(main())
