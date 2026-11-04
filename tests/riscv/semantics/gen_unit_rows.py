#!/usr/bin/env python3
"""
Generate tests/riscv/RiscvSemanticRows64.inc and RiscvSemanticRows32.inc: the rows of tests/TestRiscvSemantics.cpp.

A row is an instruction (or a unit of several), a start state and the state a reference processor ends in. The
rows are picked from the semantics corpus: for every mnemonic a few lines, for every line the states which end in
different ways (the exit, the registers it changes). Each state is run on QEMU and on Spike, and a row is kept only
if the two processors agree, so a row is something the RISC-V reference implementations both do. The unit test then
runs the row on the interpreter without any processor.

  out/venv/bin/python tests/riscv/semantics/gen_unit_rows.py [--per-mnemonic 2] [--states 3]

Row format (the strings of RiscvSemanticRow in the test):
  code      the hex bytes of the instructions, in memory order
  text      the instruction text
  setup     `name=value` tokens: x1..x31, f0..f31, fcsr, ssp, elp, cfi (1: Zicfilp / Zicfiss enabled),
            mem=ADDR:HEXBYTES,ADDR:HEXBYTES (the bytes over the memory pattern)
  expected  exit=fall|branch|trap:ecall|trap:ebreak|trap:illegal|trap:fault|trap:swcheck and the registers which
            changed, pc (a branch), mem= (the bytes written)
"""
import argparse
import collections
import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import rvasm
import rvstate as S
import states_rv
import oracle_qemu
import oracle_spike

REPO = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
OUT = os.path.join(REPO, 'tests', 'riscv')
XLEN = {'rv64': 64, 'rv32': 32}
SKIP = None


def load_skips():
    path = os.path.join(HERE, 'skip.txt')
    rules = []
    for raw in open(path):
        raw = raw.rstrip('\n')
        if not raw.strip() or raw.lstrip().startswith('#'):
            continue
        m = re.match(r'(\[[^\]]*\]\s*)?(.*?)\s+#\s', raw + ' # ')
        names = m.group(1)
        if names and any(n.strip() in ('unicorn',) for n in names.strip('[] ').split(',')):
            continue
        if names and not any(n.strip() in ('rv64', 'rv32') for n in names.strip('[] ').split(',')) and \
                any(n.strip().startswith(('rv_', 'rv64_', 'rv32_')) for n in names.strip('[] ').split(',')):
            continue
        try:
            rules.append(re.compile(m.group(2)))
        except re.error:
            pass
    return rules


def corpus_lines(arch):
    paths = sorted(set(glob.glob(os.path.join(HERE, 'corpus', 'rv_*.s')) + glob.glob(os.path.join(HERE, 'corpus', arch + '_*.s'))))
    for path in paths:
        stem = os.path.basename(path)[:-2]
        if stem.endswith(('_v', '_v2')) or '_v_' in stem or 'oracle_diffs' in stem:
            continue
        for rec in rvasm.parse_corpus(path):
            yield stem, rec


def base_mnemonic(text):
    first = text.split(';')[0].strip().split()[0].lower()
    return re.sub(r'\.(aqrl|aq|rl)$', '', first)


def registers_of(text):
    info = states_rv.analyze(text)
    return info


def setup_tokens(text, state, xlen):
    info = states_rv.analyze(text)
    tokens = []
    xs = set(info['x'])
    # the implicit registers of the control flow integrity instructions
    if state.get('cfi'):
        xs |= {1, 5, 7}
    for i in sorted(xs):
        if i != 0 and state['x'][i]:
            tokens.append('x%d=%x' % (i, state['x'][i]))
    for i in sorted(info['f']):
        tokens.append('f%d=%x' % (i, state['f'][i]))
    if state['fcsr']:
        tokens.append('fcsr=%x' % state['fcsr'])
    if state.get('cfi'):
        tokens.append('cfi=1')
        tokens.append('ssp=%x' % state['ssp'])
        if state['elp']:
            tokens.append('elp=1')
    # the memory the instruction can touch: around the addresses it computes (offset + base)
    bases = []
    for mn, ops in info['insns']:
        for op in ops:
            m = re.fullmatch(r'(-?(?:0x)?[0-9a-fA-F]*)\(\s*([a-z0-9]+)\s*\)', op.strip())
            if not m:
                continue
            reg = states_rv.reg_x(m.group(2))
            if reg is None:
                continue
            offset = int(m.group(1), 0) if m.group(1) not in ('', '-') else 0
            bases.append((state['x'][reg] + offset) & S.mask(xlen))
    if state.get('cfi'):
        bases.append(state['ssp'])
        bases.append(state['ssp'] - 16)
    mem = {}
    for addr, byte in state['mem'].items():
        if any(b - 24 <= addr < b + 40 for b in bases):
            mem[addr] = byte
    if mem:
        tokens.append('mem=' + S.format_mem(mem))
    return ' '.join(tokens)


def expected_tokens(state, res, xlen):
    out = ['exit=' + res['exit']]
    for i in range(1, 32):
        if res['x'][i] != state['x'][i]:
            out.append('x%d=%x' % (i, res['x'][i]))
    for i in range(32):
        if res['f'][i] != state['f'][i]:
            out.append('f%d=%x' % (i, res['f'][i]))
    if (res['fcsr'] & 0xff) != (state['fcsr'] & 0xff):
        out.append('fcsr=%x' % (res['fcsr'] & 0xff))
    if state.get('cfi'):
        if res['ssp'] != state['ssp']:
            out.append('ssp=%x' % res['ssp'])
        if res['elp'] != state['elp']:
            out.append('elp=%x' % res['elp'])
    elif res['elp']:
        out.append('elp=%x' % res['elp'])
    if res['exit'] == 'branch':
        out.append('pc=%x' % res['pc'])
    mem = {a: b for a, b in res.get('mem', {}).items() if S.initial_byte(state, a) != b}
    if mem:
        out.append('mem=' + S.format_mem(mem))
    return ' '.join(out)


def signature(state, res):
    changed = tuple(i for i in range(1, 32) if res['x'][i] != state['x'][i])
    changedf = tuple(i for i in range(32) if res['f'][i] != state['f'][i])
    return (res['exit'], changed, changedf, res['fcsr'] != state['fcsr'], bool(res.get('mem')))


def cstring(text):
    return '"' + text.replace('\\', '\\\\').replace('"', '\\"') + '"'


def literal(text):
    """A C string literal, split in adjacent literals under the limit of MSVC (16380 characters)."""
    pieces = [text[i:i + 8000] for i in range(0, max(len(text), 1), 8000)] or ['']
    return ' '.join(cstring(p) for p in pieces)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--per-mnemonic', type=int, default=2)
    ap.add_argument('--states', type=int, default=3)
    ap.add_argument('--candidates', type=int, default=12)
    ap.add_argument('--jobs', type=int, default=8)
    args = ap.parse_args()
    skips = load_skips()

    for arch in ('rv64', 'rv32'):
        xlen = XLEN[arch]
        chosen = collections.OrderedDict()
        # directed lines first: they hold the special values
        lines = sorted(corpus_lines(arch), key=lambda sr: (0 if '_dir_' in sr[0] else 1, sr[0], sr[1]['no']))
        counts = collections.Counter()
        per_text = {}
        for stem, rec in lines:
            text = rec['text']
            if any(r.search(text) for r in skips):
                continue
            key = base_mnemonic(text)
            mode = (rec['tags'].get('mode', [''])[0].split() or [''])[0] or None
            if counts[key] >= args.per_mnemonic * 2 and not text.startswith('c.'):
                continue
            if counts[key] >= args.per_mnemonic * 2:
                continue
            ident = (text, mode)
            if ident in chosen:
                continue
            if ';' in text and counts[key] >= 1 and 'sc.' not in text:
                continue
            chosen[ident] = (stem, rec)
            counts[key] += 1
        items = list(chosen.items())
        print('%s: %d lines' % (arch, len(items)))
        assembled = rvasm.assemble(xlen, [t for (t, m), _ in items])
        tests = []
        owners = []
        for ((text, mode), (stem, rec)), (hexb, err) in zip(items, assembled):
            if err or not hexb:
                continue
            code = bytes.fromhex(hexb)
            fixed = rec['tags'].get('state', [])
            states = states_rv.make_states(xlen, 128, text, args.candidates, None, mode, fixed,
                                           boxed=False)
            for st in states:
                tests.append((code, st))
                owners.append((text, mode, code, st))
        print('  %d candidate states' % len(tests))
        qemu = oracle_qemu.run_tests(xlen, 128, tests, jobs=args.jobs)
        spike = oracle_spike.run_tests(xlen, 128, tests, jobs=args.jobs)
        rows = collections.OrderedDict()
        seen = collections.defaultdict(set)
        for (text, mode, code, st), (qk, q), (sk, s) in zip(owners, qemu, spike):
            if qk != 'ok' or sk != 'ok':
                continue
            if S.compare_results(code, st, q, s, ('qemu', 'spike')):
                continue
            ident = (text, mode)
            sig = signature(st, q)
            if sig in seen[ident] or len([1 for r in rows.get(ident, [])]) >= args.states:
                continue
            seen[ident].add(sig)
            setup = setup_tokens(text, st, xlen)
            expected = expected_tokens(st, q, xlen)
            rows.setdefault(ident, []).append((code.hex(), text, setup, expected))
        lines_out = []
        for ident, rs in rows.items():
            for code, text, setup, expected in rs:
                lines_out.append('    { %s, %s, %s, %s }, \\' % (literal(code), literal(text), literal(setup), literal(expected)))
        name = 'RiscvSemanticRows%d.inc' % xlen
        with open(os.path.join(OUT, name), 'w') as f:
            f.write('// Generated by tests/riscv/semantics/gen_unit_rows.py: the rows of tests/TestRiscvSemantics.cpp.\n')
            f.write('// { "code", "text", "setup", "expected" }, from QEMU and Spike (they agree on every row)\n')
            f.write('#define RISCV%d_SEMANTIC_ROWS \\\n' % xlen)
            f.write('\n'.join(lines_out) + '\n')
            f.write('    { NULL, NULL, NULL, NULL }\n')
        print('  %d rows -> %s' % (len(lines_out), name))


if __name__ == '__main__':
    main()
