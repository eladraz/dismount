#!/usr/bin/env python3
"""
IA-32 semantics driver: dismount's IR execution vs. the Unicorn CPU emulator
(see tests/semantics/TEST_PLAN.md).

  out/venv/bin/python tests/semantics/run_semantics.py [--file NAME[,NAME...]]
          [--grep REGEX] [--bits 16|32] [--states N] [--jobs N] [-v]
          [--strict] [--baseline OLD.json] [--oracle-only] [--set eax=1,ecx=0]
          [--cli PATH] [--out DIR]

Per corpus line: nasm gives the bytes, N random states are drawn (seeded from
the line text), `dismount_cli run` and Unicorn (oracle.py) execute the
instruction from every state, and the exit, registers, eip, eflags and the
written memory are compared. Writes OUT/report.json, OUT/summary.md and
OUT/failures.txt. Exit status is 0 unless the harness breaks (see --strict
and --baseline).
"""
import argparse
import collections
import glob
import json
import multiprocessing
import os
import random
import re
import subprocess
import sys
import tempfile
import time
import zlib

TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(TESTS_DIR, '..', '..'))
IA32_DIR = os.path.join(REPO, 'tests', 'ia32')
sys.path.insert(0, TESTS_DIR)
sys.path.insert(0, IA32_DIR)

import run_compat as rc                                    # noqa: E402  (nasm, corpus helpers)
try:
    import oracle                                          # noqa: E402
except ImportError as e:
    sys.stderr.write('%s\nSetup: python3 -m venv out/venv && '
                     'out/venv/bin/pip install -r tests/semantics/requirements.txt\n' % e)
    sys.exit(2)

DEFAULT_CLI = os.path.join(REPO, 'out', 'bin', 'dismount_cli')
DEFAULT_OUT = os.path.join(REPO, 'out', 'semantics')
SKIP_FILE = os.path.join(TESTS_DIR, 'skip.txt')
ORACLE_BUGS_FILE = os.path.join(TESTS_DIR, 'oracle_bugs.txt')
SEM_CORPUS = os.path.join(TESTS_DIR, 'corpus')
IA32_CORPUS = os.path.join(IA32_DIR, 'corpus')
DEFAULT_IA32_FILES = ['00_addressing32', '01_addressing16', '02_prefixes', '03_immediates',
                      '04_branches', '05_bits16', 'i386', 'i486', 'pentium', 'p6']

REGS = oracle.REGS
EDGE = [0, 0, 1, 0x7f, 0x80, 0xff, 0x7fff, 0x8000, 0xffff, 0x7fffffff, 0x80000000, 0xffffffff]
FLAG_BITS = [0x1, 0x4, 0x10, 0x40, 0x80, 0x800, 0x400]     # CF PF AF ZF SF OF DF
REP_WORDS = {'rep', 'repe', 'repne', 'repz', 'repnz'}


# ---------------------------------------------------------------------------
# skip.txt
# ---------------------------------------------------------------------------

def load_skips(path):
    """Lines: `<regex>  # <reason>`; '#' lines are comments. The regex is
    searched in the instruction text."""
    skips = []
    if not os.path.exists(path):
        return skips
    with open(path, encoding='utf-8') as f:
        for no, raw in enumerate(f, 1):
            line = raw.rstrip('\n')
            if not line.strip() or line.lstrip().startswith('#'):
                continue
            rx, sep, reason = line.rpartition(' # ')
            if not sep or not reason.strip() or not rx.strip():
                sys.stderr.write('%s:%d: skip line needs "<regex>  # <reason>"\n' % (path, no))
                sys.exit(2)
            skips.append((re.compile(rx.strip()), reason.strip()))
    return skips


# ---------------------------------------------------------------------------
# States
# ---------------------------------------------------------------------------

def stable_seed(bits, text):
    return zlib.crc32(('%d:%s' % (bits, text)).encode('utf-8'))


def draw_value(rng):
    r = rng.random()
    if r < 0.45:
        return rng.choice(EDGE)
    if r < 0.80:
        return rng.getrandbits(32)
    if r < 0.90:
        return rng.getrandbits(16)
    return rng.getrandbits(8)


def make_states(bits, text, count, overrides):
    rng = random.Random(stable_seed(bits, text))
    prefixes, mnemonic, _ = rc.split_prefixes(text)
    rep_like = any(p.lower() in REP_WORDS for p in prefixes) or \
        mnemonic.startswith('loop') or mnemonic in ('jcxz', 'jecxz')
    states = []
    for _ in range(count):
        st = {n: draw_value(rng) for n in REGS}
        if rep_like or rng.random() < 0.5:
            st['ecx'] = rng.randint(0, 17)
            if not rep_like and rng.random() < 0.15:
                st['ecx'] = rng.choice([31, 32, 33, 63])        # shift/rotate counts
        fl = 0x202
        for b in FLAG_BITS:
            if rng.random() < 0.5:
                fl |= b
        st['eflags'] = fl
        st['eip'] = 0x1000
        st.update(overrides)
        states.append(st)
    return states


def parse_overrides(text):
    out = {}
    for part in filter(None, text.split(',')):
        k, _, v = part.partition('=')
        if k not in REGS + ['eip', 'eflags']:
            sys.stderr.write('--set: unknown register %r\n' % k)
            sys.exit(2)
        out[k] = int(v, 16)
    return out


# ---------------------------------------------------------------------------
# Oracle (parallel)
# ---------------------------------------------------------------------------

def oracle_task(task):
    bits, hexb, states = task
    code = bytes.fromhex(hexb)
    out = []
    ora = oracle.get_oracle(bits)
    for st in states:
        try:
            out.append(('ok', ora.run(code, st)))
        except oracle.OracleError as e:
            out.append(('err', str(e)))
        except Exception as e:                                  # Unicorn misbehaving
            out.append(('err', 'unicorn: %s: %s' % (type(e).__name__, e)))
    return out


def run_oracle(tasks, jobs):
    if jobs <= 1 or len(tasks) < 2 * jobs:
        return [oracle_task(t) for t in tasks]
    ctx = multiprocessing.get_context('fork' if hasattr(os, 'fork') else 'spawn')
    with ctx.Pool(jobs) as pool:
        return pool.map(oracle_task, tasks, chunksize=max(1, len(tasks) // (jobs * 16)))


# ---------------------------------------------------------------------------
# Comparison
# ---------------------------------------------------------------------------

def exit_class(ex):
    return 'flow' if ex in ('fall', 'branch') else ex


def mem_value(side, addr, code, eip):
    if addr in side:
        return side[addr]
    off = (addr - eip) & oracle.M32
    if off < len(code):
        return code[off]
    return oracle.pattern_byte(addr)


def compare(code, state, d, o):
    """d: parsed dismount line, o: oracle result. Returns the list of diffs."""
    diffs = []
    ed, eo = d['exit'], o['exit']
    if exit_class(ed) != exit_class(eo):
        diffs.append('exit: dismount=%s oracle=%s' % (ed, eo))
        return diffs
    both_trap = ed.startswith('trap:')
    for n in REGS:
        if d['regs'][n] != o['regs'][n]:
            diffs.append('%s: dismount=%08x oracle=%08x' % (n, d['regs'][n], o['regs'][n]))
    if not both_trap and d['eip'] != o['eip']:
        diffs.append('eip: dismount=%08x oracle=%08x' % (d['eip'], o['eip']))
    mask = oracle.EFLAGS_MASK & ~d['undef']
    if (d['eflags'] ^ o['eflags']) & mask:
        diffs.append('eflags: dismount=%08x oracle=%08x differ=%08x (undef=%08x)' % (
            d['eflags'], o['eflags'], (d['eflags'] ^ o['eflags']) & mask, d['undef']))
    for a in sorted(set(d['mem']) | set(o['mem'])):
        vd = mem_value(d['mem'], a, code, state['eip'])
        vo = mem_value(o['mem'], a, code, state['eip'])
        if vd != vo:
            diffs.append('mem[%08x]: dismount=%02x oracle=%02x' % (a, vd, vo))
            if len(diffs) > 24:
                diffs.append('...')
                break
    return diffs


# ---------------------------------------------------------------------------
# Lines
# ---------------------------------------------------------------------------

def corpus_paths(wanted):
    paths = []
    names = DEFAULT_IA32_FILES if not wanted else \
        [os.path.basename(p)[:-4] for p in sorted(glob.glob(os.path.join(IA32_CORPUS, '*.asm')))]
    for name in names:
        p = os.path.join(IA32_CORPUS, name + '.asm')
        if os.path.exists(p):
            paths.append(p)
    paths += sorted(glob.glob(os.path.join(SEM_CORPUS, '*.asm')))
    if wanted:
        paths = [p for p in paths if os.path.basename(p) in wanted]
    return paths


def collect(args, skips, tmp):
    """Parses and assembles the corpus. Returns the list of line records."""
    wanted = set()
    for item in args.file:
        for part in item.split(','):
            if part:
                wanted.add(os.path.basename(part))
                wanted.add(os.path.basename(part) + '.asm')
    pat = re.compile(args.grep) if args.grep else None
    recs = []
    for path in corpus_paths(wanted):
        info = rc.parse_corpus(path)
        if args.bits and info['bits'] != args.bits:
            continue
        lines = [r for r in info['lines'] if not pat or pat.search(r['text'])]
        if not lines:
            continue
        ref = rc.nasm_assemble('nasm', [r['text'] for r in lines], info['bits'], tmp)
        for r, (hexb, err) in zip(lines, ref):
            rec = {'file': info['name'], 'line': r['no'], 'source': r['text'],
                   'bits': info['bits'], 'hex': hexb,
                   'mnemonic': rc.split_prefixes(r['text'])[1],
                   'status': None, 'message': '', 'diffs': []}
            if err is not None:
                rec['status'], rec['message'] = 'ASM_ERR', 'nasm: ' + err
            elif not hexb:
                rec['status'], rec['message'] = 'ASM_ERR', 'nasm produced no bytes'
            else:
                for rx, reason in skips:
                    if rx.search(r['text']):
                        rec['status'], rec['message'] = 'SKIP', reason
                        break
            recs.append(rec)
    return recs


def check_cli_has_run(cli, bits):
    out = rc.run_cli(cli, 'run', bits, [oracle.format_state(
        b'\x90', {'eax': 0, 'ecx': 0, 'edx': 0, 'ebx': 0, 'esp': 0x8000, 'ebp': 0,
                  'esi': 0, 'edi': 0, 'eip': 0x1000, 'eflags': 0x202})])
    return bool(out) and out[0].startswith('OK ')


def process_mode(bits, recs, args, ctx):
    """Runs the active records of one mode. Fills rec['status'] etc."""
    active = [r for r in recs if r['status'] is None]
    if not active:
        return
    for r in active:
        r['code'] = bytes.fromhex(r['hex'])
        r['states'] = make_states(bits, r['source'], args.states, ctx['overrides'])
    t0 = time.time()

    # 1. dismount
    if not args.oracle_only:
        inputs = []
        for r in active:
            inputs += [oracle.format_state(r['code'], st) for st in r['states']]
        outs = rc.run_cli(ctx['cli'], 'run', bits, inputs)
        pos = 0
        for r in active:
            n = len(r['states'])
            r['dis_lines'] = outs[pos:pos + n]
            r['dis'] = [oracle.parse_result(x) for x in r['dis_lines']]
            pos += n
        for r in active:
            errs = [d['err'] for d in r['dis'] if 'err' in d]
            if errs:
                r['status'], r['message'] = 'DIS_ERR', errs[0][:200]
            elif any(d['exit'] in ('undefined',) or d['exit'].startswith('helper') for d in r['dis']):
                r['status'] = 'UNLIFTED'
                r['message'] = r['dis'][0]['exit']
        sys.stderr.write('  bits %d: dismount_cli %d states in %.1fs\n' % (bits, len(inputs), time.time() - t0))
    t1 = time.time()

    # 2. the oracle (only for the lines that still matter)
    todo = [r for r in active if r['status'] is None]
    results = run_oracle([(bits, r['hex'], r['states']) for r in todo], args.jobs)
    for r, res in zip(todo, results):
        r['ora'] = res
    sys.stderr.write('  bits %d: unicorn %d lines in %.1fs\n' % (bits, len(todo), time.time() - t1))

    # 3. compare
    for r in todo:
        if args.oracle_only:
            continue
        good = 0
        skipped = collections.Counter()
        for st, d, (kind, o), dl in zip(r['states'], r['dis'], r['ora'], r['dis_lines']):
            if kind != 'ok':
                skipped[o] += 1
                continue
            good += 1
            diffs = compare(r['code'], st, d, o)
            if diffs:
                r['diffs'].append({'state': oracle.format_state(r['code'], st),
                                   'dismount': dl, 'oracle': oracle.format_result(o, d['undef']),
                                   'diff': diffs})
        r['oracle_skipped'] = dict(skipped)
        if good == 0:
            r['status'] = 'ORACLE_ERR'
            r['message'] = next(iter(skipped), '')
        elif r['diffs']:
            r['status'] = 'FAIL'
            r['message'] = '%d/%d states differ: %s' % (len(r['diffs']), good, r['diffs'][0]['diff'][0])
            for rx, reason in ctx['oracle_bugs']:
                if rx.search(r['source']):
                    r['status'], r['message'] = 'ORACLE_BUG', reason + ' (' + r['message'] + ')'
                    break
        else:
            r['status'] = 'PASS'


# ---------------------------------------------------------------------------
# Reports
# ---------------------------------------------------------------------------

STATUSES = ['PASS', 'FAIL', 'UNLIFTED', 'SKIP', 'ORACLE_BUG', 'ASM_ERR', 'DIS_ERR', 'ORACLE_ERR']


def write_reports(recs, out_dir, args):
    os.makedirs(out_dir, exist_ok=True)
    totals = collections.Counter(r['status'] for r in recs)
    clean = []
    for r in recs:
        c = {k: v for k, v in r.items() if k not in ('code', 'states', 'dis', 'dis_lines', 'ora')}
        c['diffs'] = r['diffs'][:5]
        clean.append(c)
    report = {'generated': time.strftime('%Y-%m-%d %H:%M:%S'), 'states': args.states,
              'totals': dict(totals), 'lines': clean}
    with open(os.path.join(out_dir, 'report.json'), 'w') as f:
        json.dump(report, f, indent=1)

    with open(os.path.join(out_dir, 'failures.txt'), 'w') as f:
        for r in recs:
            if r['status'] not in ('FAIL', 'DIS_ERR', 'ORACLE_ERR'):
                continue
            f.write('== %s:%d | %s | bits %d | %s | %s\n' % (
                r['file'], r['line'], r['source'], r['bits'], r['hex'], r['status']))
            if r['status'] != 'FAIL':
                f.write('   %s\n' % r['message'])
            for dd in r['diffs'][:3]:
                f.write('   state:    %s\n   dismount: %s\n   oracle:   %s\n' % (
                    dd['state'], dd['dismount'], dd['oracle']))
                for x in dd['diff']:
                    f.write('   diff:     %s\n' % x)
            if len(r['diffs']) > 3:
                f.write('   (%d more failing states)\n' % (len(r['diffs']) - 3))

    per_mn = collections.OrderedDict()
    for r in recs:
        per_mn.setdefault(r['mnemonic'] or '?', collections.Counter())[r['status']] += 1
    md = ['# IA-32 semantics summary (dismount IR vs. Unicorn)', '',
          'Generated %s, %d states per line, %d lines.' % (report['generated'], args.states, len(recs)), '',
          '| status | lines |', '|---|---|']
    md += ['| %s | %d |' % (s, totals.get(s, 0)) for s in STATUSES]
    fails = sorted({m for m, c in per_mn.items() if c.get('FAIL')})
    md += ['', '## FAIL mnemonics', '', ', '.join(fails) if fails else '(none)', '',
           '## Per mnemonic', '', '| mnemonic | ' + ' | '.join(STATUSES) + ' |',
           '|---|' + '---|' * len(STATUSES)]
    for m in sorted(per_mn):
        md.append('| %s | %s |' % (m, ' | '.join(str(per_mn[m].get(s, 0) or '') for s in STATUSES)))
    with open(os.path.join(out_dir, 'summary.md'), 'w') as f:
        f.write('\n'.join(md) + '\n')
    return totals, fails


def rec_key(r):
    return '%d|%s' % (r['bits'], r['source'])


def main():
    ap = argparse.ArgumentParser(description='dismount IR vs Unicorn differential test')
    ap.add_argument('--file', action='append', default=[],
                    help='corpus file (name or path, repeatable or comma-separated)')
    ap.add_argument('--grep', default='', help='regex on the instruction text')
    ap.add_argument('--bits', type=int, choices=(16, 32), default=0, help='only lines of this mode')
    ap.add_argument('--states', type=int, default=16, help='random states per line (16)')
    ap.add_argument('--strict', action='store_true', help='exit 1 on any FAIL or DIS_ERR')
    ap.add_argument('--baseline', default='', help='OLD report.json: exit 1 if a PASS line regressed')
    ap.add_argument('-v', '--verbose', action='store_true',
                    help='print every state and both results of the failing lines')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 1, help='Unicorn worker processes')
    ap.add_argument('--oracle-only', action='store_true', help='run only Unicorn and print its results')
    ap.add_argument('--set', default='', help='override registers in every state, e.g. ecx=0,eax=5 (hex)')
    ap.add_argument('--cli', default=DEFAULT_CLI)
    ap.add_argument('--out', default=DEFAULT_OUT)
    args = ap.parse_args()

    ctx = {'cli': os.path.abspath(args.cli), 'overrides': parse_overrides(args.set),
           'oracle_bugs': load_skips(ORACLE_BUGS_FILE)}
    if not args.oracle_only and not os.path.exists(ctx['cli']):
        sys.stderr.write('dismount_cli not found: %s (build with -DDISMOUNT_BUILD_TESTS=ON)\n' % ctx['cli'])
        return 1
    skips = load_skips(SKIP_FILE)
    t_start = time.time()
    with tempfile.TemporaryDirectory() as tmp:
        recs = collect(args, skips, tmp)
    sys.stderr.write('  %d lines assembled in %.1fs\n' % (len(recs), time.time() - t_start))
    if not recs:
        sys.stderr.write('no lines selected\n')
        return 1

    if not args.oracle_only:
        for bits in (16, 32):
            if any(r['bits'] == bits and r['status'] is None for r in recs) and \
                    not check_cli_has_run(ctx['cli'], bits):
                sys.stderr.write('%s does not support `run --bits %d` (the probe did not answer OK)\n'
                                 % (ctx['cli'], bits))
                return 1
    for bits in (16, 32):
        process_mode(bits, [r for r in recs if r['bits'] == bits], args, ctx)

    if args.oracle_only:
        for r in recs:
            print('[%s:%d bits %d] %s   (%s)' % (r['file'], r['line'], r['bits'], r['source'], r['hex']))
            if r['status'] is not None:
                print('   %s %s' % (r['status'], r['message']))
                continue
            for st, (kind, o) in zip(r['states'], r['ora']):
                print('   ' + oracle.format_state(r['code'], st).partition(';')[2].strip())
                print('   -> ' + (oracle.format_result(o) if kind == 'ok' else 'ORACLE_ERR ' + o))
        return 0

    totals, fails = write_reports(recs, os.path.abspath(args.out), args)
    if args.verbose:
        for r in recs:
            if r['status'] == 'FAIL':
                print('FAIL %s:%d %s (%s)' % (r['file'], r['line'], r['source'], r['hex']))
                for st, d, (kind, o), dl in zip(r['states'], r['dis'], r['ora'], r['dis_lines']):
                    print('   state:    ' + oracle.format_state(r['code'], st).partition(';')[2].strip())
                    print('   dismount: ' + dl)
                    print('   oracle:   ' + (oracle.format_result(o, d.get('undef', 0)) if kind == 'ok'
                                             else 'ORACLE_ERR ' + o))
                    if kind == 'ok':
                        for x in compare(r['code'], st, d, o):
                            print('   diff:     ' + x)
    print('dismount vs unicorn: %d lines, %d states each, %.1fs' % (
        len(recs), args.states, time.time() - t_start))
    print('  ' + '  '.join('%s=%d' % (s, totals.get(s, 0)) for s in STATUSES))
    if fails:
        print('  FAIL mnemonics (%d): %s' % (len(fails), ' '.join(fails[:60]) + (' ...' if len(fails) > 60 else '')))
    print('reports: %s/{report.json,summary.md,failures.txt}' % os.path.abspath(args.out))

    rc_ = 0
    if args.baseline:
        with open(args.baseline) as f:
            old = json.load(f)
        was = {rec_key(r): r['status'] for r in old['lines']}
        regress = [r for r in recs if was.get(rec_key(r)) == 'PASS' and r['status'] != 'PASS']
        for r in regress:
            print('REGRESSION %s:%d %s -> %s %s' % (r['file'], r['line'], r['source'], r['status'], r['message']))
        print('baseline: %d regression(s)' % len(regress))
        if regress:
            rc_ = 1
    if args.strict and (totals.get('FAIL', 0) or totals.get('DIS_ERR', 0)):
        print('strict: %d FAIL, %d DIS_ERR' % (totals.get('FAIL', 0), totals.get('DIS_ERR', 0)))
        rc_ = 1
    return rc_


if __name__ == '__main__':
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
