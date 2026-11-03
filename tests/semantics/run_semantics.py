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
    import states_v2                                       # noqa: E402
    import x87host                                         # noqa: E402
    import x87states                                       # noqa: E402
except ImportError as e:
    sys.stderr.write('%s\nSetup: python3 -m venv out/venv && '
                     'out/venv/bin/pip install -r tests/semantics/requirements.txt\n' % e)
    sys.exit(2)

DEFAULT_CLI = os.path.join(REPO, 'out', 'bin', 'dismount_cli')
DEFAULT_OUT = os.path.join(REPO, 'out', 'semantics')
HOST_QUIRKS_FILE = os.path.join(TESTS_DIR, 'host_x87_quirks.txt')
SKIP_FILE = os.path.join(TESTS_DIR, 'skip.txt')
ORACLE_BUGS_FILE = os.path.join(TESTS_DIR, 'oracle_bugs.txt')
SEM_CORPUS = os.path.join(TESTS_DIR, 'corpus')
IA32_CORPUS = os.path.join(IA32_DIR, 'corpus')
DEFAULT_IA32_FILES = ['00_addressing32', '01_addressing16', '02_prefixes', '03_immediates',
                      '04_branches', '05_bits16', 'i386', 'i486', 'pentium', 'p6', 'x87',
                      'mmx', 'sse', 'sse2']

REGS = oracle.REGS
EDGE = [0, 0, 1, 0x7f, 0x80, 0xff, 0x7fff, 0x8000, 0xffff, 0x7fffffff, 0x80000000, 0xffffffff]
FLAG_BITS = [0x1, 0x4, 0x10, 0x40, 0x80, 0x800, 0x400]     # CF PF AF ZF SF OF DF
MXCSR_FLAGS = 0x3f
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
            if not line.strip() or line.lstrip().startswith(('#', '@')):
                continue
            rx, sep, reason = line.rpartition(' # ')
            if not sep or not reason.strip() or not rx.strip():
                sys.stderr.write('%s:%d: skip line needs "<regex>  # <reason>"\n' % (path, no))
                sys.exit(2)
            skips.append((re.compile(rx.strip()), reason.strip()))
    return skips


def load_masks(path):
    """Lines of oracle_bugs.txt: `@fsw-mask=<hex bits> <regex>  # <reason>` and
    `@eflags-mask=<hex bits> <regex>  # <reason>`. Those bits of the x87 status word / EFLAGS
    are not compared for the lines that match: Unicorn is wrong about them, but the rest of the
    line is still compared (so it can PASS). Returns {'fsw': [(bits, regex, reason)], 'eflags': [...]}"""
    masks = {'fsw': [], 'eflags': []}
    if not os.path.exists(path):
        return masks
    with open(path, encoding='utf-8') as f:
        for no, raw in enumerate(f, 1):
            line = raw.rstrip('\n')
            m = re.match(r'^@(fsw|eflags)-mask=([0-9a-fA-F]+) (.*)$', line)
            if not m:
                continue
            rx, sep, reason = m.group(3).rpartition(' # ')
            if not sep or not reason.strip() or not rx.strip():
                sys.stderr.write('%s:%d: mask line needs "@fsw-mask=BITS <regex>  # <reason>"\n' % (path, no))
                sys.exit(2)
            masks[m.group(1)].append((int(m.group(2), 16), re.compile(rx.strip()), reason.strip()))
    return masks


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


def apply_fixed(state, spec):
    """A copy of `state` with the tokens of a corpus `@state:` comment applied
    (hex numbers, the names of the `dismount_cli run` protocol)."""
    st = dict(state)
    for key in ('xmm', 'fpr', 'seg', 'mem'):
        if key in st:
            st[key] = type(st[key])(st[key])
    for tok in spec.split():
        k, _, v = tok.partition('=')
        if k in oracle.SEGS:
            st.setdefault('seg', {})[k] = int(v, 16)
        elif k.startswith('xmm') and len(k) == 4:
            st.setdefault('xmm', [0] * 8)[int(k[3])] = int(v, 16)
        elif k[0] == 'r' and len(k) == 2 and k[1].isdigit():
            st.setdefault('fpr', [0] * 8)[int(k[1])] = int(v, 16)
        elif k == 'mem':
            st.setdefault('mem', {}).update(oracle.parse_mem(v))
        else:
            st[k] = int(v, 16)
    return st


ROSETTA_SOURCE = os.path.join(TESTS_DIR, 'rosetta_probe.c')
ROSETTA_BINARY = os.path.join(REPO, 'out', 'rosetta_probe')


def build_rosetta_probe():
    """Compile tests/semantics/rosetta_probe.c as an x86-64 program."""
    if not os.path.exists(ROSETTA_BINARY) or \
            os.path.getmtime(ROSETTA_BINARY) < os.path.getmtime(ROSETTA_SOURCE):
        os.makedirs(os.path.dirname(ROSETTA_BINARY), exist_ok=True)
        cmd = ['clang', '-arch', 'x86_64', '-O1', '-o', ROSETTA_BINARY, ROSETTA_SOURCE] \
            if sys.platform == 'darwin' else ['cc', '-O1', '-o', ROSETTA_BINARY, ROSETTA_SOURCE]
        subprocess.check_call(cmd)
    return ROSETTA_BINARY


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

def is_x87_line(bits, text):
    """The lines of the x87 instructions (fxsave / fxrstor included) of the 32 bit mode."""
    return bits == 32 and re.match(r'^(f[a-z0-9]+)\b', text) is not None


def oracle_task(task):
    bits, hexb, states, use_host = task
    code = bytes.fromhex(hexb)
    out = []
    ora = x87host.get_host() if use_host else oracle.get_oracle(bits)
    for st in states:
        try:
            res = ora.run(code, st)
            if not use_host and bits == 32 and code[:2] == b'\x0f\xae':
                x87host.fix_unicorn_fxsave(code, st, res)
            out.append(('ok', res))
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


def mem_value(side, addr, code, state, bits):
    """The byte at `addr` after a run: what the run wrote, otherwise the initial
    byte (the code at cs_base + eip, mem=, the GDT, the memory pattern)."""
    if addr in side:
        return side[addr]
    code_at = (oracle.segment_base(bits, state.get('seg', {}).get('cs', 0)) + state['eip']) & oracle.M32
    off = (addr - code_at) & oracle.M32
    if off < len(code):
        return code[off]
    if addr in state.get('mem', {}):
        return state['mem'][addr]
    if bits == 32 and oracle.GDT_ADDRESS <= addr < oracle.GDT_ADDRESS + len(oracle.GDT):
        return oracle.GDT[addr - oracle.GDT_ADDRESS]
    return oracle.pattern_byte(addr)


def compare(code, state, d, o, bits=32, fsw_ignore=0, eflags_ignore=0):
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
    mask = oracle.EFLAGS_MASK & ~d['undef'] & ~eflags_ignore
    if (d['eflags'] ^ o['eflags']) & mask:
        diffs.append('eflags: dismount=%08x oracle=%08x differ=%08x (undef=%08x)' % (
            d['eflags'], o['eflags'], (d['eflags'] ^ o['eflags']) & mask, d['undef']))
    diffs += compare_v2(bits, d, o, fsw_ignore)
    for a in sorted(set(d['mem']) | set(o['mem'])):
        vd = mem_value(d['mem'], a, code, state, bits)
        vo = mem_value(o['mem'], a, code, state, bits)
        if vd != vo:
            diffs.append('mem[%08x]: dismount=%02x oracle=%02x' % (a, vd, vo))
            if len(diffs) > 24:
                diffs.append('...')
                break
    return diffs


def compare_v2(bits, d, o, fsw_ignore=0):
    """The selectors, the segment bases, the x87 and the SSE state."""
    diffs = []
    for n in oracle.SEGS:
        if d['seg'][n] != o['seg'][n]:
            diffs.append('%s: dismount=%04x oracle=%04x' % (n, d['seg'][n], o['seg'][n]))
        else:
            # Unicorn doesn't give the bases: they must be those of the selector
            try:
                want = oracle.segment_base(bits, o['seg'][n])
            except oracle.OracleError:
                continue
            if d['base'][n] != want:
                diffs.append('%s_base: dismount=%08x selector %04x has %08x' % (
                    n, d['base'][n], o['seg'][n], want))
    if d['fcw'] != o['fcw']:
        diffs.append('fcw: dismount=%04x oracle=%04x' % (d['fcw'], o['fcw']))
    if (d['fsw'] ^ o['fsw']) & ~d['fundef'] & ~fsw_ignore & 0xffff:
        diffs.append('fsw: dismount=%04x oracle=%04x (fundef=%04x)' % (d['fsw'], o['fsw'], d['fundef']))
    if d['ftw'] != o['ftw']:
        diffs.append('ftw: dismount=%02x oracle=%02x' % (d['ftw'], o['ftw']))
    for i in range(8):
        # The value of an empty register isn't defined
        if (o['ftw'] >> i) & 1 and (d['ftw'] >> i) & 1 and d['fpr'][i] != o['fpr'][i]:
            diffs.append('r%d: dismount=%020x oracle=%020x' % (i, d['fpr'][i], o['fpr'][i]))
    # The exception flags (bits 0-5) are not compared with Unicorn: QEMU 5 keeps
    # them in its softfloat status and never writes them to MXCSR
    # (oracle_bugs.txt). The --rosetta oracle reports them
    ignored = 0 if oracle._backend['rosetta'] else MXCSR_FLAGS
    if (d['mxcsr'] ^ o['mxcsr']) & ~ignored & 0xffffffff:
        diffs.append('mxcsr: dismount=%08x oracle=%08x' % (d['mxcsr'], o['mxcsr']))
    for i in range(8):
        if d['xmm'][i] != o['xmm'][i]:
            diffs.append('xmm%d: dismount=%032x oracle=%032x' % (i, d['xmm'][i], o['xmm'][i]))
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


def read_annotations(path):
    """{line number: the text after `@x87:`} of a corpus file (see x87states.py)."""
    out = {}
    with open(path, encoding='utf-8') as f:
        for no, raw in enumerate(f, 1):
            m = re.search(r';\s*(?:@feature:\s*\S+\s*)?@x87:\s*(.*?)\s*$', raw)
            if m:
                out[no] = m.group(1)
    return out


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
        annotations = read_annotations(path)
        for r in info['lines']:
            r['annotation'] = annotations.get(r['no'])
        lines = [r for r in info['lines'] if not pat or pat.search(r['text'])]
        if not lines:
            continue
        ref = rc.nasm_assemble('nasm', [r['text'] for r in lines], info['bits'], tmp)
        for r, (hexb, err) in zip(lines, ref):
            rec = {'file': info['name'], 'line': r['no'], 'source': r['text'], 'annotation': r['annotation'],
                   'match': r['text'] + (' ;; ' + r['annotation'] if r['annotation'] else ''),
                   'fixed': re.findall(r'@state:\s*([^@]*)', r.get('comment', '')),
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
        if r['annotation'] is not None:
            # a directed state: the x87 part is given, only the integer registers are random
            r['states'] = make_states(bits, r['source'], min(args.states, 4), ctx['overrides'])
            for st in r['states']:
                x87states.apply(st, r['annotation'], r['source'])
            continue
        r['states'] = make_states(bits, r['source'], args.states, ctx['overrides'])
        states_v2.add_v2(bits, r['source'], r['states'],
                         varied=r['file'].startswith('segments') or args.varied_all,
                         all_fp=args.fp_state,
                         host=args.host_x87 and is_x87_line(bits, r['source']))
        # The states of the corpus line (`; @state: xmm1=... mxcsr=...`) are
        # drawn on top of a random state
        for n, spec in enumerate(r['fixed']):
            r['states'].append(apply_fixed(r['states'][n % len(r['states'])], spec))
        # The SSE instructions with an aligned 128 bit operand: most states
        # get an aligned address, the others test the #GP
        align_rng = random.Random(stable_seed(bits, 'align:' + r['source']))
        for st in r['states']:
            oracle.align_memory_operand(bits, r['code'], st, align_rng)
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
    results = run_oracle([(bits, r['hex'], r['states'], args.host_x87 and is_x87_line(bits, r['source']))
                          for r in todo], args.jobs)
    for r, res in zip(todo, results):
        r['ora'] = res
    sys.stderr.write('  bits %d: unicorn %d lines in %.1fs\n' % (bits, len(todo), time.time() - t1))

    # 3. compare
    for r in todo:
        if args.oracle_only:
            continue
        good = 0
        use_host = args.host_x87 and is_x87_line(bits, r['source'])
        # oracle_bugs.txt is about Unicorn
        fsw_ignore, eflags_ignore = mask_of(r, ctx['host_masks'] if use_host else ctx['masks'])
        skipped = collections.Counter()
        for st, d, (kind, o), dl in zip(r['states'], r['dis'], r['ora'], r['dis_lines']):
            if kind != 'ok':
                skipped[o] += 1
                continue
            good += 1
            diffs = compare(r['code'], st, d, o, bits, fsw_ignore, eflags_ignore)
            if diffs and use_host and x87host.known_quirk(r['source'], d, o):
                diffs = []
            if diffs:
                r['diffs'].append({'state': oracle.format_state(r['code'], st),
                                   'dismount': dl, 'oracle': oracle.format_result(o, d['undef'], d['fundef']),
                                   'diff': diffs})
        r['oracle_skipped'] = dict(skipped)
        if good == 0:
            r['status'] = 'ORACLE_ERR'
            r['message'] = next(iter(skipped), '')
        elif r['diffs']:
            r['status'] = 'FAIL'
            r['message'] = '%d/%d states differ: %s' % (len(r['diffs']), good, r['diffs'][0]['diff'][0])
            for rx, reason in (ctx['host_bugs'] if use_host else ctx['oracle_bugs']):
                if rx.search(r['match']):
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
                r['file'], r['line'], r['match'], r['bits'], r['hex'], r['status']))
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


def mask_of(r, masks):
    """(x87 status word bits, EFLAGS bits) that the masks of oracle_bugs.txt (or of the host quirks)
    leave out of a line."""
    out = []
    for kind in ('fsw', 'eflags'):
        ignore = 0
        for mask, rx, reason in masks[kind]:
            if rx.search(r['match']):
                ignore |= mask
        out.append(ignore)
    return tuple(out)


def rec_key(r):
    return '%d|%s' % (r['bits'], r.get('match', r['source']))


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
    ap.add_argument('--varied-all', action='store_true',
                    help='non default segment selectors in every line (default: the segments*.asm files)')
    ap.add_argument('--fp-state', action='store_true',
                    help='random x87/SSE state in every line (default: the x87 and SIMD lines)')
    ap.add_argument('--host-x87', action='store_true',
                    help='run the x87 lines on the x87 unit of the host (a real x86 processor, or Rosetta 2 '
                         'on an Apple silicon Mac) instead of Unicorn, whose x87 has known bugs. '
                         'oracle_bugs.txt is for Unicorn and is not applied; the states are not '
                         'restricted to the cases where Unicorn is right')
    ap.add_argument('--rosetta', action='store_true',
                    help='the oracle is the host x86 FPU (tests/semantics/rosetta_probe.c, built on demand) '
                         'for the register-only SSE lines, with the MXCSR exception flags compared; '
                         'the other lines are ORACLE_ERR')
    ap.add_argument('--cli', default=DEFAULT_CLI)
    ap.add_argument('--out', default=DEFAULT_OUT)
    args = ap.parse_args()

    if args.host_x87:
        if not x87host.available():
            sys.stderr.write('--host-x87 needs an x86-64 host, or an Apple silicon Mac (Rosetta 2)\n')
            return 2
        x87host.build()
    ctx = {'cli': os.path.abspath(args.cli), 'overrides': parse_overrides(args.set),
           'oracle_bugs': load_skips(ORACLE_BUGS_FILE),
           'masks': load_masks(ORACLE_BUGS_FILE),
           'host_bugs': load_skips(HOST_QUIRKS_FILE), 'host_masks': load_masks(HOST_QUIRKS_FILE)}
    if not args.oracle_only and not os.path.exists(ctx['cli']):
        sys.stderr.write('dismount_cli not found: %s (build with -DDISMOUNT_BUILD_TESTS=ON)\n' % ctx['cli'])
        return 1
    skips = load_skips(SKIP_FILE)
    if args.rosetta:
        probe = build_rosetta_probe()
        oracle.use_rosetta(probe)
        args.compare_flags = True
    else:
        args.compare_flags = False
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
                    print('   oracle:   ' + (oracle.format_result(o, d.get('undef', 0), d.get('fundef', 0)) if kind == 'ok'
                                             else 'ORACLE_ERR ' + o))
                    if kind == 'ok':
                        for x in compare(r['code'], st, d, o, r['bits'], *mask_of(r, ctx['host_masks'] if is_x87_line(r['bits'], r['source']) and args.host_x87 else ctx['masks'])):
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
