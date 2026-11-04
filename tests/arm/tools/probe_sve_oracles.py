#!/usr/bin/env python3
"""Set the `@oracle:` tag of the lines of the SVE semantics corpora (tests/arm/semantics/corpus/
a64_sve*.s): which oracles run the instruction. A line which an oracle raises an undefined
instruction for, in every probe state, is not run by it (Unicorn is QEMU 5.0: SVE only, no SVE2, bf16,
i8mm or the later extensions; the Apple M4 runs the streaming mode: no gathers, scatters, first
fault loads or first fault register, no compact, fadda...). The tag lists the oracles which run it:
`unicorn`, `host` (both: no tag) or `none`. Lines are matched by their text; the tag of a line which
was tagged before is recomputed.

    out/venv/bin/python tests/arm/tools/probe_sve_oracles.py [--file NAME] [--states N]
"""
import argparse
import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SEM = os.path.join(HERE, '..', 'semantics')
sys.path.insert(0, SEM)

import armasm                                              # noqa: E402
import states_arm                                          # noqa: E402
import oracle_arm64                                        # noqa: E402
import oracle_host                                         # noqa: E402
import multiprocessing                                     # noqa: E402

CORPUS = os.path.join(SEM, 'corpus')


def probe(task):
    """(index, hex, [states]) -> (index, unicorn runs, host runs)"""
    index, code_hex, states_u, states_h = task
    code = bytes.fromhex(code_hex)
    results = []
    for kind, states in (('unicorn', states_u), ('host', states_h)):
        runs = False
        for st in states:
            try:
                res = (oracle_arm64.get_oracle(st.get('vl', 512)).run(code, st) if kind == 'unicorn'
                       else oracle_host.get_host().run(code, st))
            except Exception:
                continue
            if res['exit'] != 'trap:undef':
                runs = True
                break
        results.append(runs)
    return index, results[0], results[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--file', action='append', default=[])
    parser.add_argument('--states', type=int, default=3)
    args = parser.parse_args()
    oracle_host.build()
    paths = sorted(glob.glob(os.path.join(CORPUS, 'a64_sve*.s')))
    if args.file:
        paths = [p for p in paths if os.path.basename(p)[:-2] in args.file or os.path.basename(p) in args.file]
    for path in paths:
        raw = open(path).read().split('\n')
        entries = []
        for i, line in enumerate(raw):
            if not line.strip() or line.startswith('//'):
                continue
            text, _, comment = line.partition('//')
            entries.append((i, text.strip(), comment))
        texts = [t for _, t, _ in entries]
        assembled = armasm.assemble('a64', texts)
        tasks = []
        for n, ((i, text, comment), (hexb, err)) in enumerate(zip(entries, assembled)):
            if err is not None:
                continue
            fixed = re.findall(r'@state:\s*([^@]*)', comment)
            sts_u = states_arm.make_states('a64', text, args.states, {'vl': 512}, 'sve', [f.strip() for f in fixed])
            sts_h = []
            for st in sts_u:
                st = states_arm.copy_state(st)
                st['sm'] = True
                st['svl'] = st['vl']
                st['ffr'] = 0
                st['x'][18] = 0
                st['x'][28] = 0
                sts_h.append(st)
            tasks.append((n, hexb, sts_u, sts_h))
        with multiprocessing.get_context('fork').Pool(max(1, (os.cpu_count() or 2) // 2)) as pool:
            results = pool.map(probe, tasks, chunksize=4)
        runs = {n: (u, h) for n, u, h in results}
        out = []
        counts = {'both': 0, 'unicorn': 0, 'host': 0, 'none': 0}
        index_of = {i: n for n, (i, _, _) in enumerate(entries)}
        for i, line in enumerate(raw):
            if i not in index_of or index_of[i] not in runs:
                out.append(line)
                continue
            u, h = runs[index_of[i]]
            line = re.sub(r'\s*@oracle:\s*[a-z ]*?(?=\s*@|$)', '', line)
            if u and h:
                counts['both'] += 1
            else:
                tag = 'unicorn' if u else ('host' if h else 'none')
                counts[tag] += 1
                line = line + ' @oracle: ' + tag
            out.append(line)
        with open(path, 'w') as f:
            f.write('\n'.join(out))
        print('%s: %s' % (os.path.basename(path), counts))
    return 0


if __name__ == '__main__':
    sys.exit(main())
