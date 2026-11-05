#!/usr/bin/env python3
"""
Generate tests/riscv/RiscvSemanticVectorRows64.inc and RiscvSemanticVectorRows32.inc: the rows of
tests/TestRiscvSemanticsVector.cpp.

A row is a vector instruction, a start state (VLEN, vtype, vl, vstart, vxrm, vxsat and the registers the
instruction names) and the state a reference processor ends in. The rows are picked from the vector semantics
corpus (corpus/*_vgen_*.s): for a few lines of every mnemonic the states which end in different ways (the exit,
the SEW and LMUL, the registers it changes), at VLEN 128, 256 and 512 in turn. Each state is run on QEMU and on
Spike and a row is kept only if the two processors agree, so a row is something both reference implementations
do. The unit test then runs the row on the interpreter without any processor.

The memory is the pattern of the interpreter run (((uint32)(address * 2654435761) >> 24), the rows don't set
bytes) and a store row writes at most 96 bytes.

  python3 tests/riscv/semantics/gen_unit_rows_vector.py [--per-mnemonic 2] [--states 2]

Row format (the strings of RiscvVectorSemanticRow in the test, plain rows included in an initializer):
  code      the hex bytes of the instructions, in memory order
  text      the instruction text
  setup     `name=value` tokens: vlen (hex bits), vtype, vl, vstart, vxrm, vxsat, x1..x31, v0..v31 (VLEN / 4 hex
            digits)
  expected  exit=fall|trap:illegal|trap:fault and what changed: x registers, vl, vtype, vstart, vxsat, v registers,
            mem=ADDR:HEXBYTES (the bytes written)
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
VLENS = (128, 256, 512)


def corpus_lines(arch):
    paths = sorted(set(glob.glob(os.path.join(HERE, 'corpus', 'rv_vgen_*.s')) +
                       glob.glob(os.path.join(HERE, 'corpus', arch + '_vgen_*.s'))))
    for path in paths:
        stem = os.path.basename(path)[:-2]
        for rec in rvasm.parse_corpus(path):
            yield stem, rec


def cstring(text):
    return '"' + text.replace('\\', '\\\\').replace('"', '\\"') + '"'


def literal(text):
    """A C string literal, split in adjacent literals under the limit of MSVC (16380 characters)."""
    pieces = [text[i:i + 8000] for i in range(0, max(len(text), 1), 8000)] or ['']
    return ' '.join(cstring(p) for p in pieces)


def mask_register_used(text):
    return bool(re.search(r'v0\.t|, v0$|\.vvm|\.vxm|\.vim|\.vm |^vcompress|^vmerge|^vadc|^vsbc|^vm', text))


def prune_state(text, state):
    """Zero the vector registers the row doesn't carry: a row names the registers of the instruction and the groups
    they start (the LMUL, the fields of a segment, the widening and the index groups); the others are 0 in the unit
    test, so they are 0 in the states the reference processors run."""
    info = states_rv.analyze(text)
    mn = text.split()[0]
    lmul = states_rv.lmul_value(state['vtype'] & 7) or 1.0
    span = int(max(1.0, lmul))
    m = re.search(r'seg(\d)', mn)
    if m:
        span *= int(m.group(1))
    if re.match(r'v(f?w|f?n|zext|sext)', mn) or re.search(r'vf\d|ei\d', mn):
        span *= 2
    if re.search(r'ei\d', mn) or re.search(r'r(e\d+)?\.v$', mn) or mn.startswith(('vrgather', 'vcompress', 'vmv')):
        span = 8
    span = min(span, 8)
    keep = set()
    for r in info['v']:
        for k in range(span):
            if r + k < 32:
                keep.add(r + k)
    if mask_register_used(text):
        keep.add(0)
    for i in range(32):
        if i not in keep:
            state['v'][i] = 0
    return state


def is_float_text(text):
    return bool(re.match(r'vf|vmf|vfw|vfn', text))


def setup_tokens(text, state, xlen):
    info = states_rv.analyze(text)
    tokens = ['vlen=%x' % state['vlen'], 'vtype=%x' % state['vtype'], 'vl=%x' % state['vl']]
    if state['vstart']:
        tokens.append('vstart=%x' % state['vstart'])
    tokens.append('vxrm=%x' % state['vxrm'])
    if state['vxsat']:
        tokens.append('vxsat=%x' % state['vxsat'])
    for i in sorted(info['x'] - {0}):
        if state['x'][i]:
            tokens.append('x%d=%x' % (i, state['x'][i]))
    if is_float_text(text):
        for i in sorted(info['f']):
            tokens.append('f%d=%x' % (i, state['f'][i]))
        tokens.append('fcsr=%x' % (state['fcsr'] & 0xff))
    for r in range(32):
        if state['v'][r]:
            tokens.append('v%d=%0*x' % (r, state['vlen'] // 4, state['v'][r]))
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
    for k in ('vl', 'vtype', 'vstart'):
        if res[k] != state[k]:
            out.append('%s=%x' % (k, res[k]))
    if res['vxsat'] != state['vxsat']:
        out.append('vxsat=%x' % res['vxsat'])
    for i in range(32):
        if res['v'][i] != state['v'][i]:
            out.append('v%d=%0*x' % (i, state['vlen'] // 4, res['v'][i]))
    mem = {a: b for a, b in res.get('mem', {}).items() if S.initial_byte(state, a) != b}
    if mem:
        out.append('mem=' + S.format_mem(mem))
    return ' '.join(out)


def signature(state, res):
    vtype = state['vtype']
    lmul = vtype & 7
    return (res['exit'], (vtype >> 3) & 7, 0 if lmul >= 5 else (1 if lmul == 0 else 2),
            tuple(i for i in range(32) if res['v'][i] != state['v'][i])[:3], res['x'] != state['x'],
            res['fcsr'] & 0x1f, res['f'] != state['f'])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--per-mnemonic', type=int, default=2)
    ap.add_argument('--states', type=int, default=2)
    ap.add_argument('--candidates', type=int, default=24)
    ap.add_argument('--jobs', type=int, default=8)
    ap.add_argument('--only-new', action='store_true',
                    help='keep the rows of the .inc files and add the rows of the mnemonics that have none')
    args = ap.parse_args()

    for arch in ('rv64', 'rv32'):
        xlen = XLEN[arch]
        by_mnemonic = collections.OrderedDict()
        old_lines = []
        have = set()
        name = 'RiscvSemanticVectorRows%d.inc' % xlen
        if args.only_new:
            with open(os.path.join(OUT, name)) as f:
                old_lines = [l.rstrip('\n') for l in f if l.startswith('    {')]
            for l in old_lines:
                m = re.match(r'    \{ "[0-9a-f]*", "([^"]*)"', l)
                have.add(m.group(1).split()[0])
        for stem, rec in corpus_lines(arch):
            text = rec['text']
            if text.split()[0] in have:
                continue
            if rec['tags'].get('word'):
                continue                    # (the reserved encodings: the illegal rows come from the other lines)
            mn = text.split()[0]
            by_mnemonic.setdefault(mn, []).append(rec)
        chosen = []
        for mn, recs in by_mnemonic.items():
            step = max(1, len(recs) // args.per_mnemonic)
            picks = recs[::step][:args.per_mnemonic]
            if len(recs) > 3:
                # (a masked line too)
                masked = [r for r in recs if r['text'].endswith('v0.t')]
                if masked and masked[0] not in picks:
                    picks[-1] = masked[0]
            chosen.extend(picks)
        print('%s: %d lines' % (arch, len(chosen)))
        assembled = rvasm.assemble(xlen, [r['text'] for r in chosen])
        rows = []
        for vi, vlen in enumerate(VLENS):
            tests = []
            owners = []
            for k, (rec, (hexb, err)) in enumerate(zip(chosen, assembled)):
                if err or not hexb:
                    continue
                if (k + vi) % 6 != 0 and vlen != 128:
                    continue                 # (the other VLENs: one line in six)
                code = bytes.fromhex(hexb)
                fixed = rec['tags'].get('state', [])
                states = states_rv.make_states(xlen, vlen, rec['text'], args.candidates, None, None, fixed)
                for st in states:
                    st = S.copy_state(st)
                    st['mem'] = {}           # (the memory is the pattern)
                    prune_state(rec['text'], st)
                    tests.append((code, st))
                    owners.append((rec['text'], code, st))
            print('  vlen %d: %d candidate states' % (vlen, len(tests)))
            qemu = oracle_qemu.run_tests(xlen, vlen, tests, jobs=args.jobs)
            spike = oracle_spike.run_tests(xlen, vlen, tests, jobs=args.jobs)
            seen = collections.defaultdict(set)
            per_line = collections.Counter()
            for (text, code, st), (qk, q), (sk, s) in zip(owners, qemu, spike):
                if qk != 'ok' or sk != 'ok':
                    continue
                if S.compare_results(code, st, q, s, ('qemu', 'spike')):
                    continue
                if len(q.get('mem', {})) > 96:
                    continue
                if per_line[text] >= args.states:
                    continue
                sig = signature(st, q)
                if sig in seen[text]:
                    continue
                seen[text].add(sig)
                per_line[text] += 1
                rows.append((code.hex(), text, setup_tokens(text, st, xlen), expected_tokens(st, q, xlen)))
        lines_out = []
        for code, text, setup, expected in rows:
            lines_out.append('    { %s, %s, %s, %s },' % (literal(code), literal(text), literal(setup), literal(expected)))
        lines_out = old_lines + lines_out
        with open(os.path.join(OUT, name), 'w') as f:
            f.write('// Generated by tests/riscv/semantics/gen_unit_rows_vector.py: the rows of tests/TestRiscvSemanticsVector.cpp.\n')
            f.write('// { "code", "text", "setup", "expected" }, from QEMU and Spike (they agree on every row)\n')
            f.write('\n'.join(lines_out) + '\n')
        print('  %d rows -> %s' % (len(lines_out), name))


if __name__ == '__main__':
    main()
