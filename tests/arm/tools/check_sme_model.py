#!/usr/bin/env python3
"""Check the SME model (sme_model.py) against the M4: for every line of the SME semantics corpus
(tests/arm/semantics/corpus/a64_sme*.s) random states are run on the host (oracle_host.py) and in the
model, and the results are compared. The model is the expected value generator of the unit rows of
instructions the M4 does not run, so it has to agree with the M4 on the ones it does.

    check_sme_model.py [--file a64_sme] [--grep REGEX] [--states N] [-v]

Exit status 1 on a difference.

Author: Elad Raz <e@eladraz.com>
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SEM = os.path.join(HERE, '..', 'semantics')
sys.path.insert(0, HERE)
sys.path.insert(0, SEM)
import armasm        # noqa: E402
import armstate as S  # noqa: E402
import states_arm    # noqa: E402
import oracle_host   # noqa: E402
import sme_model     # noqa: E402

CORPUS = os.path.join(SEM, 'corpus')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--file', default='a64_sme')
    ap.add_argument('--grep', default='')
    ap.add_argument('--states', type=int, default=8)
    ap.add_argument('-v', action='store_true')
    args = ap.parse_args()
    pat = re.compile(args.grep) if args.grep else None
    lines = []
    for f in args.file.split(','):
        lines += [(f, r) for r in armasm.parse_corpus(os.path.join(CORPUS, f + '.s'))]
    lines = [(f, r) for f, r in lines if not pat or pat.search(r['text'])]
    asm = armasm.assemble('a64', [r['text'] for _, r in lines])
    host = oracle_host.get_host()
    bad = 0
    total = 0
    for (f, r), (hexb, err) in zip(lines, asm):
        if err:
            print('ASM_ERR %s: %s' % (r['text'], err))
            continue
        mode = (r['tags'].get('mode', ['plain'])[0].split() or ['plain'])[0]
        code = bytes.fromhex(hexb)
        states = states_arm.make_states('a64', r['text'], args.states, {'vl': 512} if mode != 'plain' else {},
                                        mode if mode != 'plain' else None, r['tags'].get('state', []))
        nbad = 0
        first = None
        for st in states:
            st = adapt(st)
            try:
                want = host.run(code, st)
            except oracle_host.OracleError as e:
                if first is None and nbad == 0:
                    first = 'host: %s' % e
                continue
            try:
                got = sme_model.run(r['text'], st)
            except NotImplementedError as e:
                first = 'model: not implemented %s' % e
                nbad += 1
                break
            total += 1
            diffs = S.compare_results('a64', code, st, got, want, ('model', 'host'), {'x18', 'x28'})
            if diffs:
                nbad += 1
                if first is None or args.v:
                    first = '%s\n    state: %s' % ('; '.join(diffs[:4]), S.format_state('a64', code, st)[:240])
                    if args.v:
                        print('DIFF %s: %s' % (r['text'], first))
        if nbad:
            bad += 1
            print('FAIL %s (%d/%d): %s' % (r['text'], nbad, len(states), first))
    print('%d lines, %d state comparisons, %d failing lines' % (len(lines), total, bad))
    return 1 if bad else 0


def adapt(st):
    if 'x' in st:
        st = dict(st)
        st['x'] = list(st['x'])
        st['x'][18] = 0
        st['x'][28] = 0
    return st


if __name__ == '__main__':
    sys.exit(main())
