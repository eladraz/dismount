#!/usr/bin/env python3
"""Check the independent floating point model (fpmodel.py, vfpsem.py) against Unicorn on the
instructions Unicorn runs: every line of tests/arm/semantics/corpus/a32_vfp.s that the model supports
and Unicorn does not UNDEF (single and double precision, the half precision conversions) is run from
the states of the semantics tests (states_arm.py plus the directed ones of the corpus) on both, and
the registers and the FPSCR are compared. The model is then the reference for the same
instructions in half precision and bfloat16 (gen_semantic_rows_aarch32_vfp.py).

    out/venv/bin/python tests/arm/tools/check_fpmodel.py [--grep REGEX] [--states N] [-v]
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, 'tests', 'arm', 'semantics'))
import armasm
import armstate as S
import oracle_arm32
import states_arm
import vfpsem

FPSCR_MASK = 0xFFC8009F


def model_state(st):
    return {'q': list(st['q']), 'r': list(st['r']), 'fpscr': st['fpscr'], 'apsr': st['apsr'], 'mem': {}}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--grep', default='')
    parser.add_argument('--states', type=int, default=24)
    parser.add_argument('-v', action='store_true')
    args = parser.parse_args()
    lines = armasm.parse_corpus(os.path.join(REPO, 'tests', 'arm', 'semantics', 'corpus', 'a32_vfp.s'))
    texts = [r['text'] for r in lines]
    assembled = armasm.assemble('a32', texts)
    bad = good = skipped = 0
    for r, (hexb, err) in zip(lines, assembled):
        text = r['text']
        if err or (args.grep and not re.search(args.grep, text)):
            continue
        if re.search(r'\b(vldr|vstr|vldm|vstm|vpush|vpop|fldm|fstm|vmrs|vmsr|vjcvt|vfmal|vfmsl|vdot|'
                     r'vmmla|vfmab|vfmat|bic)', text):
            skipped += 1
            continue
        if re.match(r'^vcmla\.f32 d\d+, d\d+, d\d+\[', text):
            skipped += 1                       # (the Unicorn bug of oracle_bugs.txt)
            continue
        code = bytes.fromhex(hexb)
        states = states_arm.make_states('a32', text, args.states, None, None, r['tags'].get('state', []))
        mismatches = 0
        undef = False
        for st in states:
            try:
                res = oracle_arm32.get_oracle(False).run(code, st)
            except oracle_arm32.OracleError:
                continue
            if res['exit'] != 'fall':
                undef = True
                break
            try:
                exp = vfpsem.run(text, model_state(st))
            except ValueError as e:
                skipped += 1
                undef = True
                break
            diffs = []
            for i in range(16):
                if exp['q'][i] != res['q'][i]:
                    diffs.append('q%d model=%032x unicorn=%032x' % (i, exp['q'][i], res['q'][i]))
            if (exp['fpscr'] & FPSCR_MASK) != (res['fpscr'] & FPSCR_MASK):
                diffs.append('fpscr model=%08x unicorn=%08x' % (exp['fpscr'] & FPSCR_MASK, res['fpscr'] & FPSCR_MASK))
            for i in range(13):
                if exp['r'][i] != res['r'][i]:
                    diffs.append('r%d model=%x unicorn=%x' % (i, exp['r'][i], res['r'][i]))
            if diffs:
                mismatches += 1
                if args.v or mismatches == 1:
                    print('MISMATCH %s: %s' % (text, '; '.join(diffs[:3])))
        if undef:
            continue
        if mismatches:
            bad += 1
        else:
            good += 1
    print('model vs unicorn: %d lines agree, %d lines differ, %d skipped' % (good, bad, skipped))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
