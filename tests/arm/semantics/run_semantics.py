#!/usr/bin/env python3
"""
ARM semantics driver: dismount's IR execution vs. the Unicorn CPU emulator and the host
processor (see tests/arm/semantics/TEST_PLAN.md).

  out/venv/bin/python tests/arm/semantics/run_semantics.py [--arch a32|t32|a64]
          [--oracle unicorn|host] [--file NAME[,NAME...]] [--grep REGEX] [--states N]
          [--jobs N] [-v] [--strict] [--baseline OLD.json] [--oracle-only]
          [--compare-oracles] [--vl BITS] [--set x1=5,nzcv=4] [--cli PATH] [--out DIR]

Per corpus line: clang assembles the bytes, N random states are drawn (seeded from the line
text), `dismount_cli run --arch ARCH` and the oracle execute the code from every state, and the
exit, the registers, the flags, the vectors, the floating point status and the written memory
are compared. Writes OUT/report.json, OUT/summary.md and OUT/failures.txt. The exit status is 0
unless the harness breaks (see --strict and --baseline).

Modes of the tool:
  --oracle-only      print the result of the oracle for every state (no dismount_cli needed)
  --compare-oracles  run the same states on Unicorn and on the host (A64 only) and report where
                     they disagree: the evidence for oracle_bugs.txt. No dismount_cli
  (default)          dismount_cli against the oracle. While `dismount_cli run --arch` doesn't
                     exist, every line is DIS_ERR (clean), the oracles are still exercised
"""
import argparse
import collections
import glob
import json
import multiprocessing
import os
import re
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..', '..'))
sys.path.insert(0, HERE)

try:
    import armasm                                          # noqa: E402
    import armstate as S                                   # noqa: E402
    import states_arm                                      # noqa: E402
except ImportError as e:
    sys.stderr.write('%s\n' % e)
    sys.exit(2)

DEFAULT_CLI = os.path.join(REPO, 'out', 'bin', 'dismount_cli')
DEFAULT_OUT = os.path.join(REPO, 'out', 'semantics_arm')
CORPUS = os.path.join(HERE, 'corpus')
SKIP_FILE = os.path.join(HERE, 'skip.txt')
ORACLE_BUGS_FILE = os.path.join(HERE, 'oracle_bugs.txt')
STATUSES = ['PASS', 'FAIL', 'UNLIFTED', 'SKIP', 'ORACLE_BUG', 'ASM_ERR', 'DIS_ERR', 'ORACLE_ERR']
HOST_IGNORE = {'x18', 'x28'}


def lazy_oracles():
    """The oracle modules are imported when they are used (Unicorn is only in the venv)."""
    global oracle_arm64, oracle_arm32, oracle_host
    import oracle_arm64                                    # noqa: F401
    import oracle_arm32                                    # noqa: F401
    import oracle_host                                     # noqa: F401


# ---------------------------------------------------------------------------
# skip.txt / oracle_bugs.txt:  `[a64,a32] <regex>  # <reason>`
# ---------------------------------------------------------------------------

def rule_applies(arches, arch, kind):
    """True if a rule `[a64,unicorn] ...` applies to the architecture `arch` and the oracle `kind`
    ('unicorn', 'host' or 'both'): the names of the architectures and of the oracles can be mixed
    in the brackets, a rule with none of one sort applies to all of them."""
    if arches is None:
        return True
    arch_names = arches & {'a64', 'a32', 't32'}
    oracle_names = arches & {'unicorn', 'host'}
    if arch_names and arch not in arch_names:
        return False
    if oracle_names and kind not in oracle_names and kind != 'both':
        return False
    return True


def load_rules(path):
    """Lines: `<regex>  # <reason>`; '#' lines are comments. The regex is searched in the
    instruction text. A leading `[a64] ` (or `[a32,t32] `...) limits the rule to those
    architectures. Returns [(regex, reason, arches or None)]."""
    rules = []
    if not os.path.exists(path):
        return rules
    with open(path, encoding='utf-8') as f:
        for no, raw in enumerate(f, 1):
            line = raw.rstrip('\n')
            if not line.strip() or line.lstrip().startswith(('#', '@')):
                continue
            rx, sep, reason = line.rpartition(' # ')
            if not sep or not reason.strip() or not rx.strip():
                sys.stderr.write('%s:%d: rule needs "<regex>  # <reason>"\n' % (path, no))
                sys.exit(2)
            arches = None
            m = re.match(r'^\[([a-z0-9,]+)\] (.*)$', rx.strip())
            if m:
                arches = set(m.group(1).split(','))
                rx = m.group(2)
            rules.append((re.compile(rx.strip()), reason.strip(), arches))
    return rules


def rule_applies(arches, arch, oracle):
    """A rule applies to the architectures and the oracles its `[a64,unicorn]` prefix names (the
    tokens `unicorn` and `host` are oracles, the others architectures; none: every one)."""
    if arches is None:
        return True
    architectures = arches - {'unicorn', 'host'}
    oracles = arches & {'unicorn', 'host'}
    return (not architectures or arch in architectures) and (not oracles or oracle in oracles)


# ---------------------------------------------------------------------------
# Corpus
# ---------------------------------------------------------------------------

def corpus_files(arch, wanted):
    paths = sorted(glob.glob(os.path.join(CORPUS, arch + '_*.s')))
    if wanted:
        paths = [p for p in paths if os.path.basename(p) in wanted or os.path.basename(p)[:-2] in wanted]
    return paths


def collect(args, skips):
    wanted = set()
    for item in args.file:
        for part in item.split(','):
            if part:
                wanted.add(os.path.basename(part))
    pat = re.compile(args.grep) if args.grep else None
    recs = []
    for path in corpus_files(args.arch, wanted):
        lines = [r for r in armasm.parse_corpus(path) if not pat or pat.search(r['text'])]
        if not lines:
            continue
        ref = armasm.assemble(args.arch, [r['text'] for r in lines])
        for r, (hexb, err) in zip(lines, ref):
            modes = r['tags'].get('mode', ['plain'])
            rec = {'file': os.path.basename(path)[:-2], 'line': r['no'], 'source': r['text'],
                   'arch': args.arch, 'mode': (modes[0].split() or ['plain'])[0], 'hex': hexb,
                   'feature': (r['tags'].get('feature') or [''])[0],
                   'fixed': r['tags'].get('state', []),
                   'mnemonic': r['text'].split()[0].lower() if r['text'].split() else '',
                   'status': None, 'message': '', 'diffs': []}
            oracles = (r['tags'].get('oracle') or [''])[0].split()
            if err is not None:
                rec['status'], rec['message'] = 'ASM_ERR', 'clang: ' + err
            elif oracles and args.oracle not in oracles and not args.lift_only:
                rec['status'] = 'SKIP'
                rec['message'] = 'not run by %s (@oracle: %s, see oracle_bugs.txt)' % (args.oracle, ' '.join(oracles))
            else:
                for rx, reason, arches in skips:
                    if rule_applies(arches, args.arch, args.oracle) and rx.search(r['text']):
                        rec['status'], rec['message'] = 'SKIP', reason
                        break
            recs.append(rec)
    return recs


# ---------------------------------------------------------------------------
# States
# ---------------------------------------------------------------------------

def oracle_vl(args, kind):
    """The SVE vector length of the 'sve' mode lines for an oracle: Unicorn runs --vl, the host
    runs them in streaming mode at its SVL (512)."""
    if kind == 'host' or args.compare_oracles:
        return 512
    return args.vl


def make_states(rec, args, kind, overrides):
    mode = rec['mode']
    ov = dict(overrides)
    if rec['arch'] == 'a64' and mode in ('sve', 'sm', 'za'):
        ov['vl'] = oracle_vl(args, kind)
    states = states_arm.make_states(rec['arch'], rec['source'], args.states, ov,
                                    mode if mode != 'plain' else None, rec['fixed'])
    if kind == 'host':
        states = [adapt_for_host(st) for st in states]
    return states


def adapt_for_host(st):
    """The host has no SVE outside of the streaming mode: the 'sve' mode lines run in
    streaming mode at the same vector length (512), without ffr."""
    if st.get('sve') and not st.get('sm'):
        st = states_arm.copy_state(st)
        st['sm'] = True
        st['svl'] = st['vl']
        st['ffr'] = 0
    # x18 is the platform register and x28 the pointer of the runner: zero, not compared
    if 'x' in st:
        st = states_arm.copy_state(st)
        st['x'][18] = 0
        st['x'][28] = 0
    return st


# ---------------------------------------------------------------------------
# Oracles (parallel)
# ---------------------------------------------------------------------------

def oracle_task(task):
    kind, arch, hexb, states, vl = task
    lazy_oracles()
    code = bytes.fromhex(hexb)
    out = []
    for st in states:
        try:
            if kind == 'host':
                res = oracle_host.get_host().run(code, st)
            elif arch == 'a64':
                res = oracle_arm64.get_oracle(st.get('vl', vl) if st.get('sve') else vl).run(code, st)
            else:
                res = oracle_arm32.get_oracle(arch == 't32').run(code, st)
            out.append(('ok', res))
        except (oracle_arm64.OracleError, oracle_arm32.OracleError, oracle_host.OracleError) as e:
            out.append(('err', str(e)))
        except Exception as e:                                  # an engine misbehaving
            out.append(('err', '%s: %s: %s' % (kind, type(e).__name__, e)))
    return out


def run_oracle(tasks, jobs):
    if jobs <= 1 or len(tasks) < 2:
        return [oracle_task(t) for t in tasks]
    ctx = multiprocessing.get_context('fork' if hasattr(os, 'fork') else 'spawn')
    with ctx.Pool(jobs) as pool:
        return pool.map(oracle_task, tasks, chunksize=1)


# ---------------------------------------------------------------------------
# dismount_cli
# ---------------------------------------------------------------------------

def run_cli(cli, arch, inputs):
    """Runs `dismount_cli run --arch ARCH`. Returns one output line per input; the lines of a
    CLI that doesn't know the command are `ERR ...`."""
    proc = subprocess.run([cli, 'run', '--arch', arch], input=''.join(s + '\n' for s in inputs),
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
    got = proc.stdout.splitlines()
    if len(got) < len(inputs) or not all(g.startswith(('OK ', 'ERR ')) for g in got[:1]):
        tail = (proc.stderr.strip().splitlines() or [''])[-1][:160]
        got = ['ERR dismount_cli run --arch %s not available (rc=%d) %s' % (arch, proc.returncode, tail)] * len(inputs)
    return got


# ---------------------------------------------------------------------------
# Running one mode
# ---------------------------------------------------------------------------

def line_ignore(args, kind):
    return HOST_IGNORE if kind == 'host' else set()


def process(recs, args, ctx):
    active = [r for r in recs if r['status'] is None]
    if not active:
        return
    kind = args.oracle
    for r in active:
        r['code'] = bytes.fromhex(r['hex'])
        r['states'] = make_states(r, args, kind, ctx['overrides'])
    t0 = time.time()

    # 1. dismount
    if not args.oracle_only:
        inputs = []
        for r in active:
            inputs += [S.format_state(r['arch'], r['code'], st) for st in r['states']]
        outs = run_cli(ctx['cli'], args.arch, inputs)
        pos = 0
        for r in active:
            n = len(r['states'])
            r['dis_lines'] = outs[pos:pos + n]
            r['dis'] = [S.parse_result(r['arch'], x) for x in r['dis_lines']]
            pos += n
        for r in active:
            errs = [d['err'] for d in r['dis'] if 'err' in d]
            if errs:
                r['status'], r['message'] = 'DIS_ERR', errs[0][:200]
            elif any(d['exit'] == 'undefined' or d['exit'].startswith('helper') for d in r['dis']):
                r['status'] = 'UNLIFTED'
                r['message'] = r['dis'][0]['exit']
        sys.stderr.write('  dismount_cli %d states in %.1fs\n' % (len(inputs), time.time() - t0))
        if args.lift_only:
            # No oracle: the lifted lines are SKIP (not run against a processor), the unlifted ones UNLIFTED
            for r in active:
                if r['status'] is None:
                    r['status'], r['message'] = 'SKIP', 'lift only: lifted'
            return
    t1 = time.time()

    # 2. the oracle (the lines that still matter; with --oracle-only all of them)
    todo = [r for r in active if r['status'] is None or args.oracle_only]
    results = run_oracle([(kind, r['arch'], r['hex'], r['states'], args.vl) for r in todo], args.jobs)
    for r, res in zip(todo, results):
        r['ora'] = res
    sys.stderr.write('  %s %d lines in %.1fs\n' % (kind, len(todo), time.time() - t1))
    if args.oracle_only:
        return

    # 3. compare
    ignore = line_ignore(args, kind)
    for r in todo:
        good = 0
        skipped = collections.Counter()
        for st, d, (k, o), dl in zip(r['states'], r['dis'], r['ora'], r['dis_lines']):
            if k != 'ok':
                skipped[o] += 1
                continue
            good += 1
            diffs = S.compare_results(r['arch'], r['code'], st, d, o, ('dismount', kind), ignore)
            if diffs and kind == 'host':
                # The M4 sometimes returns a run with the exception flags of FPSR set (all of them,
                # 0x0800009f) whatever the instruction: not reproducible. A state which differs is run
                # again (twice at most); a difference which stays is a difference
                for _ in range(2):
                    try:
                        again = oracle_host.get_host().run(r['code'], st)
                    except Exception:
                        break
                    retry = S.compare_results(r['arch'], r['code'], st, d, again, ('dismount', kind), ignore)
                    if not retry:
                        diffs = retry
                        r['host_retries'] = r.get('host_retries', 0) + 1
                        break
                    o, diffs = again, retry
            if diffs:
                r['diffs'].append({'state': S.format_state(r['arch'], r['code'], st),
                                   'dismount': dl, 'oracle': S.format_result(r['arch'], o),
                                   'diff': diffs})
        r['oracle_skipped'] = dict(skipped)
        if good == 0:
            r['status'] = 'ORACLE_ERR'
            r['message'] = next(iter(skipped), '')
        elif r['diffs']:
            r['status'] = 'FAIL'
            r['message'] = '%d/%d states differ: %s' % (len(r['diffs']), good, r['diffs'][0]['diff'][0])
            for rx, reason, arches in ctx['oracle_bugs']:
                if rule_applies(arches, r['arch'], kind) and rx.search(r['source']):
                    r['status'], r['message'] = 'ORACLE_BUG', reason + ' (' + r['message'] + ')'
                    break
        else:
            r['status'] = 'PASS'


# ---------------------------------------------------------------------------
# Cross-check of the oracles
# ---------------------------------------------------------------------------

def compare_oracles(recs, args, ctx):
    """Runs the active A64 lines on Unicorn and on the host with the same states and reports the
    disagreements."""
    active = [r for r in recs if r['status'] is None]
    tasks_u, tasks_h = [], []
    for r in active:
        r['code'] = bytes.fromhex(r['hex'])
        base = states_arm.make_states(r['arch'], r['source'], args.states,
                                      dict(ctx['overrides'], **({'vl': 512} if r['mode'] in ('sve', 'sm', 'za') else {})),
                                      r['mode'] if r['mode'] != 'plain' else None, r['fixed'])
        r['states'] = base
        # Unicorn: the state as is (sve at vl 512 for the sve lines); host: the same state, the
        # sve lines in streaming mode
        tasks_u.append(('unicorn', r['arch'], r['hex'], [adapt_unicorn(st) for st in base], 512))
        tasks_h.append(('host', r['arch'], r['hex'], [adapt_for_host(st) for st in base], 512))
    t0 = time.time()
    ru = run_oracle(tasks_u, args.jobs)
    sys.stderr.write('  unicorn %d lines in %.1fs\n' % (len(active), time.time() - t0))
    t0 = time.time()
    rh = run_oracle(tasks_h, max(1, min(args.jobs, 4)))
    sys.stderr.write('  host %d lines in %.1fs\n' % (len(active), time.time() - t0))
    totals = collections.Counter()
    rows = []
    for r, ou, oh in zip(active, ru, rh):
        agree = differ = 0
        details = []
        for st, (ku, a), (kh, b) in zip(r['states'], ou, oh):
            if ku != 'ok' or kh != 'ok':
                totals['one oracle could not run'] += 1
                details.append(('not run', S.format_state(r['arch'], r['code'], st),
                                ['unicorn: %s' % (a if ku != 'ok' else 'ran'),
                                 'host: %s' % (b if kh != 'ok' else 'ran')]))
                continue
            ignore = set(HOST_IGNORE)
            if st.get('sve') or st.get('sm'):
                ignore.add('ffr')
            diffs = S.compare_results(r['arch'], r['code'], st, a, b, ('unicorn', 'host'), ignore)
            if diffs:
                differ += 1
                details.append(('differ', S.format_state(r['arch'], r['code'], st), diffs))
            else:
                agree += 1
        totals['agree'] += agree
        totals['differ'] += differ
        r['agree'], r['differ'], r['details'] = agree, differ, details
        if differ:
            r['status'] = 'FAIL'
            r['message'] = '%d/%d states differ: %s' % (differ, agree + differ, next(d for d in details if d[0] == 'differ')[2][0])
            for rx, reason, arches in ctx['oracle_bugs']:
                if rule_applies(arches, r['arch'], 'both') and rx.search(r['source']):
                    r['status'], r['message'] = 'ORACLE_BUG', reason + ' (' + r['message'] + ')'
                    break
        elif not agree:
            r['status'] = 'ORACLE_ERR'
            r['message'] = details[0][2][0] if details else ''
        else:
            r['status'] = 'PASS'
    return totals


def adapt_unicorn(st):
    """Unicorn: no SME. The state is run as it is (the sve lines at the vl of the state)."""
    return st


# ---------------------------------------------------------------------------
# Reports
# ---------------------------------------------------------------------------

def write_reports(recs, out_dir, args, title):
    os.makedirs(out_dir, exist_ok=True)
    totals = collections.Counter(r['status'] for r in recs)
    clean = []
    for r in recs:
        c = {k: v for k, v in r.items() if k not in ('code', 'states', 'dis', 'dis_lines', 'ora', 'details')}
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
            if args.compare_oracles:
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
    with open(os.path.join(out_dir, 'summary.md'), 'w') as f:
        f.write('\n'.join(md) + '\n')
    return totals, fails


def rec_key(r):
    return '%s|%s' % (r['arch'], r['source'])


def summarize(arch, st, res):
    """The registers an oracle result changed (against the input state), as text."""
    out = ['exit=' + res['exit']]
    if arch == 'a64':
        names = [('x%d' % i, st['x'][i], res['x'][i]) for i in range(31)]
        names += [('sp', st['sp'], res['sp']), ('nzcv', st['nzcv'], res['nzcv']),
                  ('fpcr', st['fpcr'], res['fpcr']), ('fpsr', st['fpsr'], res['fpsr'])]
        names += [('v%d' % i, st['v'][i], res['v'][i]) for i in range(32)]
        if (res.get('sve') or res.get('sm')) and 'z' in res:
            names += [('z%d' % i, st['z'][i], res['z'][i]) for i in range(32)]
            names += [('p%d' % i, st['p'][i], res['p'][i]) for i in range(16)]
    else:
        names = [('r%d' % i, st['r'][i], res['r'][i]) for i in range(13)]
        names += [('sp', st['sp'], res['sp']), ('lr', st['lr'], res['lr']), ('apsr', st['apsr'], res['apsr']),
                  ('fpscr', st['fpscr'], res['fpscr'])]
        names += [('q%d' % i, st['q'][i], res['q'][i]) for i in range(16)]
    for n, a, b in names:
        if a != b:
            out.append('%s=%x' % (n, b))
    out.append('pc=%x' % res['pc'])
    if res.get('mem'):
        out.append('mem=' + S.format_mem(res['mem']))
    return ' '.join(out)


def print_oracle_only(recs, args):
    for r in recs:
        print('[%s:%d %s] %s   (%s)' % (r['file'], r['line'], r['arch'], r['source'], r['hex']))
        if r['status'] is not None and 'ora' not in r:
            print('   %s %s' % (r['status'], r['message']))
            continue
        for st, (kind, o) in zip(r['states'], r['ora']):
            if args.verbose:
                print('   in:  ' + S.format_state(r['arch'], r['code'], st).partition(';')[2].strip()[:600])
            print('   -> ' + (summarize(r['arch'], st, o)[:400] if kind == 'ok' else 'ORACLE_ERR ' + o))


def main():
    ap = argparse.ArgumentParser(description='dismount ARM IR vs Unicorn / the host')
    ap.add_argument('--arch', choices=('a32', 't32', 'a64'), default='a64')
    ap.add_argument('--oracle', choices=('unicorn', 'host'), default='unicorn')
    ap.add_argument('--file', action='append', default=[], help='corpus file (name, repeatable or comma-separated)')
    ap.add_argument('--grep', default='', help='regex on the instruction text')
    ap.add_argument('--states', type=int, default=16, help='random states per line (16)')
    ap.add_argument('--strict', action='store_true', help='exit 1 on any FAIL or DIS_ERR')
    ap.add_argument('--baseline', default='', help='OLD report.json: exit 1 if a PASS line regressed')
    ap.add_argument('-v', '--verbose', action='store_true', help='print the failing states')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 1, help='oracle worker processes')
    ap.add_argument('--oracle-only', action='store_true', help='print the results of the oracle only')
    ap.add_argument('--compare-oracles', action='store_true', help='Unicorn vs the host (a64)')
    ap.add_argument('--lift-only', action='store_true',
                    help='run dismount_cli only (every line, whatever its @oracle tag): UNLIFTED or SKIP; '
                         'the report says which lines are lifted (tests/arm/tools/gen_support_matrix.py)')
    ap.add_argument('--vl', type=int, default=128, help='SVE vector length of Unicorn (128..512)')
    ap.add_argument('--set', default='', help='override registers in every state, e.g. x1=5,nzcv=4 (hex)')
    ap.add_argument('--cli', default=DEFAULT_CLI)
    ap.add_argument('--out', default=DEFAULT_OUT)
    args = ap.parse_args()
    if args.compare_oracles and args.arch != 'a64':
        sys.stderr.write('--compare-oracles needs --arch a64 (the host has no AArch32)\n')
        return 2
    lazy_oracles()
    if args.oracle == 'host' or args.compare_oracles:
        if not oracle_host.available():
            sys.stderr.write('the host oracle needs an arm64 macOS host\n')
            return 2
        oracle_host.build()
        if args.arch != 'a64':
            sys.stderr.write('--oracle host needs --arch a64\n')
            return 2

    overrides = {}
    for part in filter(None, args.set.split(',')):
        k, _, v = part.partition('=')
        if re.fullmatch(r'x\d+', k):
            overrides.setdefault('x', {})[int(k[1:])] = int(v, 16)
        elif re.fullmatch(r'r\d+', k):
            overrides.setdefault('r', {})[int(k[1:])] = int(v, 16)
        else:
            overrides[k] = int(v, 16)
    ctx = {'cli': os.path.abspath(args.cli), 'overrides': overrides,
           'oracle_bugs': load_rules(ORACLE_BUGS_FILE)}
    skips = load_rules(SKIP_FILE)
    t_start = time.time()
    recs = collect(args, skips)
    sys.stderr.write('  %d lines assembled in %.1fs\n' % (len(recs), time.time() - t_start))
    if not recs:
        sys.stderr.write('no lines selected\n')
        return 1

    if args.compare_oracles:
        totals_o = compare_oracles(recs, args, ctx)
        totals, fails = write_reports(recs, os.path.abspath(args.out), args, 'ARM semantics: Unicorn vs host')
        states_total = totals_o['agree'] + totals_o['differ']
        print('unicorn vs host (%s): %d lines, %d states each, %.1fs' % (args.arch, len(recs), args.states, time.time() - t_start))
        print('  states compared: %d, agree: %d, differ: %d, one oracle could not run: %d' % (
            states_total, totals_o['agree'], totals_o['differ'], totals_o['one oracle could not run']))
        print('  ' + '  '.join('%s=%d' % (s, totals.get(s, 0)) for s in STATUSES))
        if fails:
            print('  differing mnemonics (%d): %s' % (len(fails), ' '.join(fails[:60])))
        if args.verbose:
            for r in recs:
                for kind, state, diffs in r.get('details', []):
                    print('%s %s:%d %s\n   state: %s' % (kind.upper(), r['file'], r['line'], r['source'], state[:300]))
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

    totals, fails = write_reports(recs, os.path.abspath(args.out), args, 'ARM semantics summary (dismount IR vs %s)' % args.oracle)
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
    print('dismount vs %s (%s): %d lines, %d states each, %.1fs' % (args.oracle, args.arch, len(recs), args.states, time.time() - t_start))
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
