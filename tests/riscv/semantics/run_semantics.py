#!/usr/bin/env python3
"""
RISC-V semantics driver: dismount's IR execution vs. the QEMU system emulator, Unicorn and Spike
(see tests/riscv/semantics/TEST_PLAN.md).

  out/venv/bin/python tests/riscv/semantics/run_semantics.py [--arch rv64|rv32]
          [--oracle qemu|unicorn|spike] [--vlen BITS] [--file NAME[,NAME...]] [--grep REGEX]
          [--states N] [--jobs N] [-v] [--strict] [--baseline OLD.json] [--oracle-only]
          [--cross OR1,OR2] [--set x1=5,fcsr=60] [--cli PATH] [--out DIR]

Per corpus line: llvm-mc assembles the bytes, N states are drawn (seeded from the line text),
`dismount_cli run --arch ARCH --vlen N` and the oracle execute the code from every state, and the
exit, the registers, the vector state, the floating point status and the written memory are
compared. Writes OUT/report.json, OUT/summary.md and OUT/failures.txt. The exit status is 0 unless
the harness breaks (see --strict and --baseline).

Modes of the tool:
  --oracle-only   print the result of the oracle for every state (no dismount_cli needed)
  --cross A,B     run the same states on two oracles and report where they disagree: the evidence
                  for oracle_bugs.txt. No dismount_cli
  (default)       dismount_cli against the oracle. While `dismount_cli run --arch rv64` doesn't
                  exist, every line is DIS_ERR (clean), the oracles are still exercised
"""
import argparse
import collections
import concurrent.futures
import fnmatch
import glob
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..', '..'))
sys.path.insert(0, HERE)

try:
    import rvasm                                           # noqa: E402
    import rvstate as S                                    # noqa: E402
    import states_rv                                       # noqa: E402
except ImportError as e:
    sys.stderr.write('%s\n' % e)
    sys.exit(2)

DEFAULT_CLI = os.path.join(REPO, 'out', 'bin', 'dismount_cli')
DEFAULT_OUT = os.path.join(REPO, 'out', 'semantics_riscv')
CORPUS = os.path.join(HERE, 'corpus')
SKIP_FILE = os.path.join(HERE, 'skip.txt')
ORACLE_BUGS_FILE = os.path.join(HERE, 'oracle_bugs.txt')
STATUSES = ['PASS', 'FAIL', 'UNLIFTED', 'SKIP', 'ORACLE_BUG', 'ASM_ERR', 'DIS_ERR', 'ORACLE_ERR']
ORACLES = ('qemu', 'unicorn', 'spike')
ARCH_XLEN = {'rv64': 64, 'rv32': 32}


def load_oracle(name):
    """The oracle modules are imported when they are used (Unicorn is only in the venv)."""
    if name == 'qemu':
        import oracle_qemu as m
    elif name == 'unicorn':
        import oracle_unicorn as m
    elif name == 'spike':
        import oracle_spike as m
    else:
        raise ValueError(name)
    return m


# ---------------------------------------------------------------------------
# skip.txt / oracle_bugs.txt:  `[rv64,qemu] <regex>  # <reason>`
# ---------------------------------------------------------------------------

def load_rules(path):
    """Lines: `<regex>  # <reason>`; '#' lines are comments. The regex is searched in the
    instruction text. A leading `[rv64] ` (or `[rv32,unicorn] `...) limits the rule to those
    architectures / oracles. Returns [(regex, reason, names or None)]."""
    rules = []
    if not os.path.exists(path):
        return rules
    with open(path, encoding='utf-8') as f:
        for no, raw in enumerate(f, 1):
            line = raw.rstrip('\n')
            if not line.strip() or line.lstrip().startswith('#'):
                continue
            rx, sep, reason = line.rpartition(' # ')
            if not sep or not reason.strip() or not rx.strip():
                sys.stderr.write('%s:%d: rule needs "<regex>  # <reason>"\n' % (path, no))
                sys.exit(2)
            names = None
            m = re.match(r'^\[([a-z0-9_,]+)\] (.*)$', rx.strip())
            if m:
                names = set(m.group(1).split(','))
                rx = m.group(2)
            rules.append((re.compile(rx.strip()), reason.strip(), names))
    return rules


def rule_applies(names, arch, oracles, file=None):
    """A rule applies to the architectures, the oracles and the corpus files its `[rv64,unicorn,rv_b]`
    prefix names (the tokens `qemu`, `unicorn`, `spike` are oracles, `rv64` / `rv32` architectures, any
    other token a corpus file name; none of one sort: every one of it). `oracles` is the set of the
    oracles of the run (two for --cross): the rule applies when it names one of them (or none)."""
    if names is None:
        return True
    architectures = names & set(ARCH_XLEN)
    named = names & set(ORACLES)
    files = names - set(ARCH_XLEN) - set(ORACLES)
    return ((not architectures or arch in architectures) and (not named or bool(named & set(oracles)))
            and (not files or file in files))


# ---------------------------------------------------------------------------
# Corpus
# ---------------------------------------------------------------------------

def corpus_files(arch, wanted):
    paths = sorted(set(glob.glob(os.path.join(CORPUS, 'rv_*.s')) + glob.glob(os.path.join(CORPUS, arch + '_*.s'))))
    if wanted:
        # (a name may be a glob: `--file 'rv*_gen_*'`)
        paths = [p for p in paths if os.path.basename(p) in wanted or os.path.basename(p)[:-2] in wanted or
                 any(fnmatch.fnmatch(os.path.basename(p)[:-2], w) for w in wanted if '*' in w)]
    return paths


def collect(args, skips, oracles):
    wanted = set()
    for item in args.file:
        for part in item.split(','):
            if part:
                wanted.add(os.path.basename(part))
    pat = re.compile(args.grep) if args.grep else None
    xlen = ARCH_XLEN[args.arch]
    recs = []
    for path in corpus_files(args.arch, wanted):
        lines = [r for r in rvasm.parse_corpus(path) if not pat or pat.search(r['text'])]
        if not lines:
            continue
        ref = rvasm.assemble(xlen, [r['text'] for r in lines])
        for r, (hexb, err) in zip(lines, ref):
            modes = r['tags'].get('mode', [''])
            rec = {'file': os.path.basename(path)[:-2], 'line': r['no'], 'source': r['text'],
                   'arch': args.arch, 'mode': (modes[0].split() or [''])[0] or None, 'hex': hexb,
                   'feature': (r['tags'].get('feature') or [''])[0],
                   'fixed': r['tags'].get('state', []),
                   'mnemonic': r['text'].split()[0].lower() if r['text'].split() else '',
                   'status': None, 'message': '', 'diffs': []}
            only = (r['tags'].get('oracle') or [''])[0].split()
            if err is not None:
                rec['status'], rec['message'] = 'ASM_ERR', 'llvm-mc: ' + err
            elif only and not (set(only) & set(oracles)):
                rec['status'] = 'SKIP'
                rec['message'] = 'not run by %s (@oracle: %s, see oracle_bugs.txt)' % ('/'.join(oracles), ' '.join(only))
            else:
                for rx, reason, names in skips:
                    if rule_applies(names, args.arch, oracles, os.path.basename(path)[:-2]) and rx.search(r['text']):
                        rec['status'], rec['message'] = 'SKIP', reason
                        break
            recs.append(rec)
    return recs


# ---------------------------------------------------------------------------
# dismount_cli
# ---------------------------------------------------------------------------

def run_cli_chunk(cli, arch, vlen, inputs):
    """One `dismount_cli run --arch ARCH --vlen N` process over `inputs`."""
    proc = subprocess.run([cli, 'run', '--arch', arch, '--vlen', str(vlen)], input=''.join(s + '\n' for s in inputs),
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
    got = proc.stdout.splitlines()
    if len(got) < len(inputs) or not all(g.startswith(('OK ', 'ERR ')) for g in got[:1]):
        tail = (proc.stderr.strip().splitlines() or [''])[-1][:160]
        got = ['ERR dismount_cli run --arch %s not available (rc=%d) %s' % (arch, proc.returncode, tail)] * len(inputs)
    return got


def run_cli(cli, arch, vlen, inputs, jobs=1):
    """Runs `dismount_cli run --arch ARCH --vlen N` (in `jobs` processes over slices of the inputs). Returns
    one output line per input; the lines of a CLI that doesn't know the command are `ERR ...`."""
    if jobs <= 1 or len(inputs) < 2000:
        return run_cli_chunk(cli, arch, vlen, inputs)
    size = (len(inputs) + jobs * 4 - 1) // (jobs * 4)
    chunks = [inputs[i:i + size] for i in range(0, len(inputs), size)]
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        parts = list(pool.map(lambda c: run_cli_chunk(cli, arch, vlen, c), chunks))
    return [line for part in parts for line in part]


# ---------------------------------------------------------------------------
# Running
# ---------------------------------------------------------------------------

def make_states(rec, args, overrides):
    xlen = ARCH_XLEN[args.arch]
    # Unicorn (QEMU 5) doesn't NaN box: the cross-check against it draws properly boxed values only
    boxed = 'unicorn' in ((args.cross.split(',') if args.cross else [args.oracle])) or args.boxed
    return states_rv.make_states(xlen, args.vlen, rec['source'], args.states, overrides, rec['mode'], rec['fixed'], boxed=boxed)


def run_oracle(name, args, recs):
    """Run the states of the `recs` on the oracle `name`, as one batch. Sets r['ora_<name>']."""
    m = load_oracle(name)
    xlen = ARCH_XLEN[args.arch]
    tests = []
    for r in recs:
        code = bytes.fromhex(r['hex'])
        tests += [(code, st) for st in r['states']]
    t0 = time.time()
    kw = {}
    if name == 'qemu' and args.qemu_cpu:
        kw['cpu'] = args.qemu_cpu
    results = m.run_tests(xlen, args.vlen, tests, jobs=args.jobs, **kw) if name != 'spike' else m.run_tests(
        xlen, args.vlen, tests, jobs=args.jobs)
    sys.stderr.write('  %s %d lines, %d states in %.1fs (%.0f states/s)\n' % (
        name, len(recs), len(tests), time.time() - t0, len(tests) / max(0.001, time.time() - t0)))
    pos = 0
    for r in recs:
        n = len(r['states'])
        r['ora_' + name] = results[pos:pos + n]
        pos += n


def f_single_result(mnemonic):
    """True for the instructions whose floating point result is a single (NaN boxed on QEMU >= 5.2 and
    on real hardware, zero extended on Unicorn)."""
    parts = mnemonic.split('.')
    return (parts[:1] != ['fcvt'] and parts[1:2] == ['s']) or (parts[:1] == ['fcvt'] and parts[1:2] == ['s']) or mnemonic in ('flw', 'fmv.w.x', 'c.flw', 'c.flwsp')


def mark_all_illegal(recs, name):
    """A line whose every state is an illegal instruction on the oracle: the oracle lacks the instruction (or the
    corpus line is wrong). Recorded as r['all_illegal'] and listed in summary.md."""
    for r in recs:
        res = r.get('ora_' + name)
        if res and all(k == 'ok' and o['exit'] == 'trap:illegal' for k, o in res):
            r['all_illegal'] = name


def _states_task(task):
    return states_rv.make_states(*task[:7], seed_extra='', boxed=task[7])


def generate_states(active, args, ctx):
    """Draw the states of the lines (in parallel: the draws are a function of the text only)."""
    xlen = ARCH_XLEN[args.arch]
    boxed = 'unicorn' in ((args.cross.split(',') if args.cross else [args.oracle])) or args.boxed
    tasks = [(xlen, args.vlen, r['source'], args.states, ctx['overrides'], r['mode'], r['fixed'], boxed) for r in active]
    t0 = time.time()
    if args.jobs > 1 and len(tasks) > 8:
        import multiprocessing
        mp = multiprocessing.get_context('fork' if hasattr(os, 'fork') else 'spawn')
        with mp.Pool(args.jobs) as pool:
            results = pool.map(_states_task, tasks, chunksize=max(1, len(tasks) // (args.jobs * 8)))
    else:
        results = [_states_task(t) for t in tasks]
    for r, states in zip(active, results):
        r['code'] = bytes.fromhex(r['hex'])
        r['states'] = states
    sys.stderr.write('  %d states drawn in %.1fs\n' % (sum(len(r['states']) for r in active), time.time() - t0))


def process(recs, args, ctx):
    active = [r for r in recs if r['status'] is None]
    if not active:
        return
    xlen = ARCH_XLEN[args.arch]
    kind = args.oracle
    generate_states(active, args, ctx)

    # 1. dismount
    if not args.oracle_only:
        t0 = time.time()
        inputs = []
        for r in active:
            inputs += [S.format_state(r['code'], st) for st in r['states']]
        outs = run_cli(ctx['cli'], args.arch, args.vlen, inputs, args.jobs)
        pos = 0
        for r in active:
            n = len(r['states'])
            r['dis_lines'] = outs[pos:pos + n]
            r['dis'] = [S.parse_result(xlen, x) for x in r['dis_lines']]
            pos += n
        for r in active:
            errs = [d['err'] for d in r['dis'] if 'err' in d]
            if errs:
                r['status'], r['message'] = 'DIS_ERR', errs[0][:200]
            elif any(d['exit'] == 'undefined' or d['exit'].startswith('helper') for d in r['dis']):
                r['status'] = 'UNLIFTED'
                r['message'] = r['dis'][0]['exit']
        sys.stderr.write('  dismount_cli %d states in %.1fs\n' % (len(inputs), time.time() - t0))

    # 2. the oracle (the lines that still matter; with --oracle-only all of them)
    todo = [r for r in active if r['status'] is None or args.oracle_only]
    run_oracle(kind, args, todo)
    mark_all_illegal(todo, kind)
    if args.oracle_only:
        return

    # 3. compare
    for r in todo:
        good = 0
        skipped = collections.Counter()
        for st, d, (k, o), dl in zip(r['states'], r['dis'], r['ora_' + kind], r['dis_lines']):
            if k != 'ok':
                skipped[o] += 1
                continue
            good += 1
            diffs = S.compare_results(r['code'], st, d, o, ('dismount', kind), (), not args.no_trap_state,
                                      kind == 'unicorn' and f_single_result(r['mnemonic']))
            if diffs:
                r['diffs'].append({'state': S.format_state(r['code'], st), 'dismount': dl,
                                   'oracle': S.format_result(o), 'diff': diffs})
        r['oracle_skipped'] = dict(skipped)
        if good == 0:
            r['status'] = 'ORACLE_ERR'
            r['message'] = next(iter(skipped), '')
        elif r['diffs']:
            r['status'] = 'FAIL'
            r['message'] = '%d/%d states differ: %s' % (len(r['diffs']), good, r['diffs'][0]['diff'][0])
            for rx, reason, names in ctx['oracle_bugs']:
                if rule_applies(names, r['arch'], [kind], r['file']) and rx.search(r['source']):
                    r['status'], r['message'] = 'ORACLE_BUG', reason + ' (' + r['message'] + ')'
                    break
        else:
            r['status'] = 'PASS'


# ---------------------------------------------------------------------------
# Cross-check of the oracles
# ---------------------------------------------------------------------------

def cross(recs, args, ctx):
    """Runs the active lines on two oracles with the same states and reports the disagreements."""
    first, second = args.cross.split(',')
    low32 = 'unicorn' in (first, second)
    active = [r for r in recs if r['status'] is None]
    generate_states(active, args, ctx)
    run_oracle(first, args, active)
    run_oracle(second, args, active)
    mark_all_illegal(active, first)
    mark_all_illegal(active, second)
    totals = collections.Counter()
    for r in active:
        agree = differ = 0
        details = []
        for st, (ka, a), (kb, b) in zip(r['states'], r['ora_' + first], r['ora_' + second]):
            if ka != 'ok' or kb != 'ok':
                totals['one oracle could not run'] += 1
                details.append(('not run', S.format_state(r['code'], st),
                                ['%s: %s' % (first, a if ka != 'ok' else 'ran'), '%s: %s' % (second, b if kb != 'ok' else 'ran')]))
                continue
            diffs = S.compare_results(r['code'], st, a, b, (first, second), (), not args.no_trap_state,
                                      low32 and f_single_result(r['mnemonic']))
            if diffs:
                differ += 1
                details.append(('differ', S.format_state(r['code'], st) + '\n   %s: %s\n   %s: %s' % (
                    first, S.format_result(a)[:300], second, S.format_result(b)[:300]), diffs))
            else:
                agree += 1
        totals['agree'] += agree
        totals['differ'] += differ
        r['agree'], r['differ'], r['details'] = agree, differ, details
        if differ:
            r['status'] = 'FAIL'
            r['message'] = '%d/%d states differ: %s' % (differ, agree + differ, next(d for d in details if d[0] == 'differ')[2][0])
            for rx, reason, names in ctx['oracle_bugs']:
                if rule_applies(names, r['arch'], [first, second], r['file']) and rx.search(r['source']):
                    r['status'], r['message'] = 'ORACLE_BUG', reason + ' (' + r['message'] + ')'
                    break
        elif not agree:
            r['status'] = 'ORACLE_ERR'
            r['message'] = details[0][2][0] if details else ''
        else:
            r['status'] = 'PASS'
    return totals


# ---------------------------------------------------------------------------
# Reports
# ---------------------------------------------------------------------------

def write_reports(recs, out_dir, args, title):
    os.makedirs(out_dir, exist_ok=True)
    totals = collections.Counter(r['status'] for r in recs)
    clean = []
    for r in recs:
        c = {k: v for k, v in r.items() if not k.startswith('ora_') and k not in ('code', 'states', 'dis', 'dis_lines', 'details')}
        c['diffs'] = r['diffs'][:5]
        clean.append(c)
    report = {'generated': time.strftime('%Y-%m-%d %H:%M:%S'), 'states': args.states,
              'arch': args.arch, 'totals': dict(totals), 'lines': clean}
    with open(os.path.join(out_dir, 'report.json'), 'w') as f:
        json.dump(report, f, indent=1)
    with open(os.path.join(out_dir, 'failures.txt'), 'w') as f:
        for r in recs:
            if r['status'] not in ('FAIL', 'DIS_ERR', 'ORACLE_ERR'):
                continue
            f.write('== %s:%d | %s | %s | %s | %s\n' % (r['file'], r['line'], r['source'], r['arch'], r['hex'], r['status']))
            if r['status'] != 'FAIL':
                f.write('   %s\n' % r['message'])
            if args.cross:
                for kind, state, diffs in [d for d in r.get('details', []) if d[0] == 'differ'][:3]:
                    f.write('   state:  %s\n' % state)
                    for x in diffs:
                        f.write('   diff:   %s\n' % x)
                continue
            for dd in r['diffs'][:3]:
                f.write('   state:    %s\n   dismount: %s\n   oracle:   %s\n' % (dd['state'], dd['dismount'], dd['oracle']))
                for x in dd['diff']:
                    f.write('   diff:     %s\n' % x)
            if len(r['diffs']) > 3:
                f.write('   (%d more failing states)\n' % (len(r['diffs']) - 3))
    per_mn = collections.OrderedDict()
    for r in recs:
        per_mn.setdefault(r['mnemonic'] or '?', collections.Counter())[r['status']] += 1
    md = ['# %s' % title, '', 'Generated %s, %d states per line, %d lines.' % (report['generated'], args.states, len(recs)), '',
          '| status | lines |', '|---|---|']
    md += ['| %s | %d |' % (s, totals.get(s, 0)) for s in STATUSES]
    fails = sorted({m for m, c in per_mn.items() if c.get('FAIL')})
    md += ['', '## FAIL mnemonics', '', ', '.join(fails) if fails else '(none)', '',
           '## Per mnemonic', '', '| mnemonic | ' + ' | '.join(STATUSES) + ' |', '|---|' + '---|' * len(STATUSES)]
    for m in sorted(per_mn):
        md.append('| %s | %s |' % (m, ' | '.join(str(per_mn[m].get(s, 0) or '') for s in STATUSES)))
    ill = [r for r in recs if r.get('all_illegal')]
    if ill:
        md += ['', '## Lines which are an illegal instruction in every state of an oracle', '']
        md += ['- %s:%d `%s` (%s)' % (r['file'], r['line'], r['source'], r['all_illegal']) for r in ill]
    with open(os.path.join(out_dir, 'summary.md'), 'w') as f:
        f.write('\n'.join(md) + '\n')
    return totals, fails


def rec_key(r):
    return '%s|%s' % (r['arch'], r['source'])


def summarize(st, res):
    """The registers an oracle result changed (against the input state), as text."""
    xlen = res['xlen']
    out = ['exit=' + res['exit']]
    d = xlen // 4
    for i in range(1, 32):
        if st['x'][i] != res['x'][i]:
            out.append('x%d=%0*x' % (i, d, res['x'][i]))
    for i in range(32):
        if st['f'][i] != res['f'][i]:
            out.append('f%d=%016x' % (i, res['f'][i]))
    if (st['fcsr'] & 0xff) != res['fcsr']:
        out.append('fcsr=%x' % res['fcsr'])
    if st['vector']:
        for k in ('vl', 'vtype', 'vstart', 'vxrm', 'vxsat'):
            if st[k] != res[k]:
                out.append('%s=%x' % (k, res[k]))
        for i in range(32):
            if st['v'][i] != res['v'][i]:
                out.append('v%d=%0*x' % (i, st['vlen'] // 4, res['v'][i]))
    out.append('pc=%x' % res['pc'])
    if res.get('mem'):
        out.append('mem=' + S.format_mem(res['mem']))
    return ' '.join(out)


def print_oracle_only(recs, args):
    kind = args.oracle
    for r in recs:
        print('[%s:%d %s] %s   (%s)' % (r['file'], r['line'], r['arch'], r['source'], r['hex']))
        if r['status'] is not None and 'ora_' + kind not in r:
            print('   %s %s' % (r['status'], r['message']))
            continue
        for st, (k, o) in zip(r['states'], r['ora_' + kind]):
            if args.verbose:
                print('   in:  ' + S.format_state(r['code'], st).partition(';')[2].strip()[:600])
            print('   -> ' + (summarize(st, o)[:400] if k == 'ok' else 'ORACLE_ERR ' + o))


def main():
    ap = argparse.ArgumentParser(description='dismount RISC-V IR vs QEMU / Unicorn / Spike')
    ap.add_argument('--arch', choices=tuple(ARCH_XLEN), default='rv64')
    ap.add_argument('--oracle', choices=ORACLES, default='qemu')
    ap.add_argument('--vlen', type=int, default=128, help='VLEN in bits (128, 256, 512)')
    ap.add_argument('--file', action='append', default=[], help='corpus file (name, repeatable or comma-separated)')
    ap.add_argument('--grep', default='', help='regex on the instruction text')
    ap.add_argument('--states', type=int, default=16, help='states per line (16)')
    ap.add_argument('--strict', action='store_true', help='exit 1 on any FAIL or DIS_ERR')
    ap.add_argument('--baseline', default='', help='OLD report.json: exit 1 if a PASS line regressed')
    ap.add_argument('-v', '--verbose', action='store_true', help='print the failing states')
    ap.add_argument('--jobs', type=int, default=min(8, os.cpu_count() or 1), help='oracle worker processes')
    ap.add_argument('--oracle-only', action='store_true', help='print the results of the oracle only')
    ap.add_argument('--cross', default='', help='compare two oracles with each other, e.g. qemu,unicorn')
    ap.add_argument('--qemu-cpu', default='', help='the QEMU cpu model (default max): rva23u64, rva23s64...')
    ap.add_argument('--boxed', action='store_true', help='draw properly NaN boxed f registers only (implied by unicorn)')
    ap.add_argument('--no-trap-state', action='store_true', help='after a trap compare the exit only')
    ap.add_argument('--set', default='', help='override registers in every state, e.g. x1=5,fcsr=60 (hex)')
    ap.add_argument('--cli', default=DEFAULT_CLI)
    ap.add_argument('--out', default=DEFAULT_OUT)
    args = ap.parse_args()
    if args.vlen not in (128, 256, 512):
        sys.stderr.write('--vlen must be 128, 256 or 512\n')
        return 2
    if args.cross:
        pair = args.cross.split(',')
        if len(pair) != 2 or any(p not in ORACLES for p in pair) or pair[0] == pair[1]:
            sys.stderr.write('--cross needs two different oracles: %s\n' % ','.join(ORACLES))
            return 2

    overrides = {}
    for part in filter(None, args.set.split(',')):
        k, _, v = part.partition('=')
        m = re.fullmatch(r'([xfv])(\d+)', k)
        if m:
            overrides.setdefault(m.group(1), {})[int(m.group(2))] = int(v, 16)
        else:
            overrides[k] = int(v, 16)
    ctx = {'cli': os.path.abspath(args.cli), 'overrides': overrides,
           'oracle_bugs': load_rules(ORACLE_BUGS_FILE)}
    skips = load_rules(SKIP_FILE)
    oracles = args.cross.split(',') if args.cross else [args.oracle]
    t_start = time.time()
    recs = collect(args, skips, oracles)
    sys.stderr.write('  %d lines assembled in %.1fs\n' % (len(recs), time.time() - t_start))
    if not recs:
        sys.stderr.write('no lines selected\n')
        return 1

    if args.cross:
        totals_o = cross(recs, args, ctx)
        totals, fails = write_reports(recs, os.path.abspath(args.out), args, 'RISC-V semantics: %s' % args.cross)
        print('%s (%s, vlen %d): %d lines, %d states each, %.1fs' % (args.cross, args.arch, args.vlen, len(recs), args.states, time.time() - t_start))
        print('  states compared: %d, agree: %d, differ: %d, one oracle could not run: %d' % (
            totals_o['agree'] + totals_o['differ'], totals_o['agree'], totals_o['differ'], totals_o['one oracle could not run']))
        print('  ' + '  '.join('%s=%d' % (s, totals.get(s, 0)) for s in STATUSES))
        if fails:
            print('  differing mnemonics (%d): %s' % (len(fails), ' '.join(fails[:80])))
        if args.verbose:
            for r in recs:
                for kind, state, diffs in r.get('details', []):
                    print('%s %s:%d %s\n   state: %s' % (kind.upper(), r['file'], r['line'], r['source'], state[:600]))
                    for x in diffs[:6]:
                        print('   ' + x[:300])
        print('reports: %s/{report.json,summary.md,failures.txt}' % os.path.abspath(args.out))
        return 0

    if not args.oracle_only and not os.path.exists(ctx['cli']):
        sys.stderr.write('dismount_cli not found: %s (build with -DDISMOUNT_BUILD_TESTS=ON)\n' % ctx['cli'])
        return 1
    process(recs, args, ctx)

    if args.oracle_only:
        print_oracle_only(recs, args)
        return 0

    totals, fails = write_reports(recs, os.path.abspath(args.out), args, 'RISC-V semantics summary (dismount IR vs %s)' % args.oracle)
    if args.verbose:
        for r in recs:
            if r['status'] == 'FAIL':
                print('FAIL %s:%d %s (%s)' % (r['file'], r['line'], r['source'], r['hex']))
                for dd in r['diffs']:
                    print('   state:    ' + dd['state'][:300])
                    print('   dismount: ' + dd['dismount'][:300])
                    print('   oracle:   ' + dd['oracle'][:300])
                    for x in dd['diff']:
                        print('   diff:     ' + x)
    print('dismount vs %s (%s, vlen %d): %d lines, %d states each, %.1fs' % (args.oracle, args.arch, args.vlen, len(recs), args.states, time.time() - t_start))
    print('  ' + '  '.join('%s=%d' % (s, totals.get(s, 0)) for s in STATUSES))
    if fails:
        print('  FAIL mnemonics (%d): %s' % (len(fails), ' '.join(fails[:60])))
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
