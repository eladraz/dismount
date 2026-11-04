#!/usr/bin/env python3
"""Generate tests/arm/AArch32SemanticRowsVfp.inc, the rows of the floating point instructions that
Unicorn (the only AArch32 processor oracle, tests/arm/semantics) cannot run: the half precision
arithmetic, compares, conversions and moves (FEAT_FP16: UNDEFINED on Unicorn), vldr.16 / vstr.16 and
the bfloat16 instructions (FEAT_BF16). The expected states are computed by the independent model of
the Arm ARM pseudocode (fpmodel.py, vfpsem.py), which agrees with Unicorn on single and double
precision (check_fpmodel.py), and not by running dismount.

The instructions and their directed states are the lines of tests/arm/semantics/corpus/a32_vfp.s
that oracle_bugs.txt marks as Unicorn's (FP16, BF16), run from the directed states of the corpus
(zeros of both signs, infinities, NaNs, denormals, the rounding modes, FZ, DN, AHP...), and the
memory forms below.

    gen_semantic_rows_aarch32_vfp.py

Row syntax: see tests/TestArmAArch32Semantics.cpp.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, 'tests', 'arm', 'semantics'))
import armasm
import armstate as S
import vfpsem

OUTPUT = os.path.join(REPO, 'tests', 'arm', 'AArch32SemanticRowsVfp.inc')
CORPUS = os.path.join(REPO, 'tests', 'arm', 'semantics', 'corpus', 'a32_vfp.s')
STATES_PER_LINE = 10

# the lines of the corpus which are not verified by Unicorn (the patterns of oracle_bugs.txt)
SELECT = [
    re.compile(r'^(?!vcvt[bt]\.f(16\.f(32|64)|(32|64)\.f16)|vcvt\.f(16\.f32|32\.f16)|vfm[as]l)\w+(\.\w+)*\.f16\b'),
    re.compile(r'^\w+(\.\w+)*\.bf16\b'),
    # the Unicorn bugs of oracle_bugs.txt
    re.compile(r'^vcmla\.f32 d\d+, d\d+, d\d+\['),
    re.compile(r'^vmsr fpscr, r0 ; vmrs r2, fpscr$'),
]


def state_tokens(state):
    out = []
    for i, q in enumerate(state['q']):
        if q:
            out.append('q%d=%032x' % (i, q))
    for i, r in enumerate(state['r']):
        if r:
            out.append('r%d=%x' % (i, r))
    if state['fpscr']:
        out.append('fpscr=%x' % state['fpscr'])
    if state['apsr']:
        out.append('apsr=%x' % state['apsr'])
    return out


def diff_tokens(before, after):
    out = []
    for i in range(16):
        if before['q'][i] != after['q'][i]:
            out.append('q%d=%032x' % (i, after['q'][i]))
    for i in range(13):
        if before['r'][i] != after['r'][i]:
            out.append('r%d=%x' % (i, after['r'][i]))
    if before['fpscr'] != after['fpscr']:
        out.append('fpscr=%x' % after['fpscr'])
    if before['apsr'] != after['apsr']:
        out.append('apsr=%x' % after['apsr'])
    return out


def new_state(tokens):
    st = S.default_state('a32')
    S.parse_tokens('a32', tokens.split(), st)
    return {'q': list(st['q']), 'r': list(st['r']), 'fpscr': st['fpscr'], 'apsr': st['apsr'], 'mem': {}}


def c_string(text):
    return '"%s"' % text.replace('\\', '\\\\').replace('"', '\\"')


def main():
    lines = armasm.parse_corpus(CORPUS)
    chosen = [r for r in lines if any(rx.match(r['text']) for rx in SELECT)]
    extra = [
        # the half precision memory forms, with a memory image
        ('vldr.16 s0, [r4, #2]', 'q0=ffffffffffffffffffffffffffffffff r4=1000 mem=1000:0011223344556677'),
        ('vldr.16 s5, [r4, #-30]', 'r4=1100 mem=10e2:aabb'),
        ('vstr.16 s0, [r4, #2]', 'q0=00000000000000000000000012345678 r4=2000'),
    ]
    texts = [r['text'] for r in chosen] + [t for t, _ in extra]
    assembled = armasm.assemble('a32', texts)
    rows = []
    for index, r in enumerate(chosen):
        hexb, err = assembled[index]
        if err:
            raise SystemExit('%s: %s' % (r['text'], err))
        states = r['tags'].get('state', [''])[:STATES_PER_LINE] or ['']
        for tokens in states:
            before = new_state(tokens)
            after = vfpsem.run(r['text'], before)
            rows.append((hexb, r['text'], ' '.join(state_tokens(before)), ' '.join(diff_tokens(before, after))))
    # the memory forms, by hand
    for k, (text, tokens) in enumerate(extra):
        hexb, err = assembled[len(chosen) + k]
        if err:
            raise SystemExit('%s: %s' % (text, err))
        mem_part = [t for t in tokens.split() if t.startswith('mem=')]
        reg_part = ' '.join(t for t in tokens.split() if not t.startswith('mem='))
        before = new_state(reg_part)
        after = new_state(reg_part)
        if text.startswith('vldr.16 s0'):
            after['q'][0] = (before['q'][0] & ~0xFFFFFFFF) | 0x3322      # [0x1002..0x1003] = 22 33
        elif text.startswith('vldr.16 s5'):
            after['q'][1] = (before['q'][1] & ~(0xFFFFFFFF << 32)) | (0xbbaa << 32)   # [0x10e2] = aa bb
        setup = ' '.join(state_tokens(before) + mem_part)
        expected = diff_tokens(before, after)
        if text.startswith('vstr'):
            expected.append('mem=2002:7856')
        rows.append((hexb, text, setup, ' '.join(expected)))
    with open(OUTPUT, 'w') as f:
        f.write('// Generated by tests/arm/tools/gen_semantic_rows_aarch32_vfp.py: the rows of the floating point instructions\n')
        f.write('// that Unicorn cannot run (half precision, bfloat16) of tests/TestArmAArch32Semantics.cpp, computed by the\n')
        f.write('// independent model of the Arm ARM pseudocode (tests/arm/tools/fpmodel.py).\n')
        f.write('// { isThumb, "code", "text", "setup", "expected" }\n')
        f.write('#define AARCH32_SEMANTIC_ROWS_VFP \\\n')
        for hexb, text, setup, expected in rows:
            f.write('    { 0, %s, %s, %s, %s }, \\\n' % (c_string(hexb), c_string(text), c_string(setup), c_string(expected)))
        f.write('\n')
    print('wrote %s: %d rows' % (OUTPUT, len(rows)))


if __name__ == '__main__':
    main()
