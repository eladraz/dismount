#!/usr/bin/env python3
"""Generate tests/arm/Arm64SmeSemanticRows.inc, the rows of tests/TestArm64SemanticsSme.cpp: an SME /
SME2 instruction, a start state and the state it must leave, as the Arm ARM defines them. The expected
values are computed by the model of tests/arm/tools/sme_model.py (an independent model of the
pseudocode, checked against the M4 by check_sme_model.py on every instruction the M4 runs), not by
running dismount. The rows cover every entry of the SME encoding table which dismount lifts and the model
supports, so the features the M4 doesn't implement (the F16F16 and B16B16 arithmetic, FAMINMAX, SVE
BFSCALE, SME2.1 movaz, SME2.2 fmul...) are checked against the pseudocode; the others are pinned.

    gen_sme_semantic_rows.py

Row syntax of the generated file, see tests/TestArm64SemanticsSme.cpp:
    { word, "text", "setup", "expected" }
The setup and expected strings are blank separated tokens `name=hex`:
    svl=<bits>              the streaming vector length (512 when absent)
    sm=<0|1> za=<0|1>       SVCR (both 1 when absent): the state of the processor; in the expectation,
                            the SVCR after the instruction
    fill=<class> seed=<n>   z0-z31, p0-p15 and the rows of za and zt0 are filled by the generator of
                            the test (splitmix64 of the seed): class int (raw bits), f16, bf16, f32, f64
                            (the elements are special numbers and normal numbers of the format)
    pred=all                every predicate bit set (after the fill)
    x<n>= sp= fpcr= fpsr=   the scalar state
    z<n>= p<n>= za<n>= zt0= explicit values (over the fill); in the expectation the registers which
                            differ from the start state
    mem=<addr>:<hex>,...    the memory (zero but for the bytes given); in the expectation the bytes
                            written
    exit=<fall|trap:undef|trap:fault>
Registers the expectation doesn't name must be unchanged.

The fill (the same code in the test): splitmix64: state += 0x9E3779B97F4A7C15; z = state;
z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9; z = (z ^ (z >> 27)) * 0x94D049BB133111EB; z ^ (z >> 31). The
order: z0..z31, p0..p15, za rows 0..svl/8 - 1, zt0. A vector takes svl / 64 outputs for the class int (64
bits each), one output per element for the float classes; a predicate takes one output (its low svl / 8
bits); zt0 takes eight outputs. A float element of E exponent bits and F fraction bits: with the output o,
if (o & 15) < 6 the special number number (o >> 4) & 15 (0 +0, 1 -0, 2 1.0, 3 -1.0, 4 2.0, 5 0.5, 6 +inf,
7 -inf, 8 quiet NaN with payload 1, 9 signaling NaN with payload 1, 10 minimum denormal, 11 negative
maximum denormal, 12 minimum normal, 13 maximum normal, 14 1.5, 15 -3.0), otherwise a normal number: the sign
bit (o >> 4) & 1, the biased exponent bias + ((o >> 8) % 41) - 20 clamped to 1..max - 1, the fraction
(o >> 16) & (2^F - 1).

Author: Elad Raz <e@eladraz.com>
"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..', '..'))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, 'tests', 'arm', 'semantics'))
import armstate as S      # noqa: E402
import sme_model          # noqa: E402

TABLE = os.path.join(REPO, 'Source', 'dismount', 'proc', 'arm', 'Arm64EncodingTableSme.cpp')
CLI = os.path.join(REPO, 'out', 'bin', 'dismount_cli')
OUTPUT = os.path.join(REPO, 'tests', 'arm', 'Arm64SmeSemanticRows.inc')
M64 = (1 << 64) - 1
MEM_SIZE = 0x20000
MEM_DATA = 0x4000
M4_FEATURES = {'FEAT_SME', 'FEAT_SME2', 'FEAT_SME_F64F64', 'FEAT_SME_I16I64'}


class Splitmix(object):
    def __init__(self, seed):
        self.state = seed & M64

    def next(self):
        self.state = (self.state + 0x9E3779B97F4A7C15) & M64
        z = self.state
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & M64
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & M64
        return z ^ (z >> 31)


FORMATS = {'f16': (5, 10), 'bf16': (8, 7), 'f32': (8, 23), 'f64': (11, 52)}


def float_element(o, cls):
    e, f = FORMATS[cls]
    bits = 1 + e + f
    bias = (1 << (e - 1)) - 1
    maxexp = (1 << e) - 1
    if (o & 15) < 6:
        t = (o >> 4) & 15
        sign = 1 if t in (1, 3, 7, 11, 15) else 0
        exp, frac = {0: (0, 0), 1: (0, 0), 2: (bias, 0), 3: (bias, 0), 4: (bias + 1, 0), 5: (bias - 1, 0),
                     6: (maxexp, 0), 7: (maxexp, 0), 8: (maxexp, (1 << (f - 1)) | 1), 9: (maxexp, 1),
                     10: (0, 1), 11: (0, (1 << f) - 1), 12: (1, 0), 13: (maxexp - 1, (1 << f) - 1),
                     14: (bias, 1 << (f - 1)), 15: (bias + 1, 1 << (f - 1))}[t]
    else:
        sign = (o >> 4) & 1
        exp = bias + ((o >> 8) % 41) - 20
        exp = max(1, min(maxexp - 1, exp))
        frac = (o >> 16) & ((1 << f) - 1)
    return (sign << (e + f)) | (exp << f) | frac


def fill_vector(rng, bits, cls):
    if cls == 'int':
        out = 0
        for i in range(bits // 64):
            out |= rng.next() << (64 * i)
        return out
    ebits = 1 + sum(FORMATS[cls])
    out = 0
    for i in range(bits // ebits):
        out |= float_element(rng.next(), cls) << (ebits * i)
    return out


def make_state(setup):
    """The start state of a setup dictionary (the model state)."""
    svl = setup.get('svl', 512)
    st = S.default_state('a64')
    st['svl'] = svl
    st['vl'] = 128
    st['sm'] = bool(setup.get('sm', 1))
    st['za'] = bool(setup.get('za', 1))
    st['sve'] = True
    st['pc'] = 0x1000
    st['sp'] = 0x8000
    st['mem_base'] = 0
    st['mem_size'] = MEM_SIZE
    st['z'] = [0] * 32
    st['p'] = [0] * 16
    st['ffr'] = 0
    st['zarows'] = [0] * (svl // 8)
    st['zt0'] = 0
    cls = setup.get('fill')
    if cls:
        rng = Splitmix(setup['seed'])
        st['z'] = [fill_vector(rng, svl, cls) for _ in range(32)]
        st['p'] = [rng.next() & ((1 << (svl // 8)) - 1) for _ in range(16)]
        st['zarows'] = [fill_vector(rng, svl, cls) for _ in range(svl // 8)]
        st['zt0'] = sum(rng.next() << (64 * i) for i in range(8))
        if setup.get('pred') == 'all':
            st['p'] = [(1 << (svl // 8)) - 1] * 16
    for key, value in setup.items():
        m = re.fullmatch(r'([xzp]|za)(\d+)', key)
        if m:
            kind, n = m.group(1), int(m.group(2))
            if kind == 'x':
                st['x'][n] = value
            elif kind == 'z':
                st['z'][n] = value
            elif kind == 'p':
                st['p'][n] = value
            else:
                st['zarows'][n] = value
        elif key == 'zt0':
            st['zt0'] = value
        elif key in ('fpcr', 'fpsr', 'sp'):
            st[key] = value
        elif key == 'mem':
            for addr, data in value:
                for i, b in enumerate(data):
                    st['mem'][addr + i] = b
    st['v'] = [z & ((1 << 128) - 1) for z in st['z']]
    return st


def tokens(d):
    parts = []
    for key, value in d.items():
        if key == 'mem':
            parts.append('mem=' + ','.join('%x:%s' % (a, bytes(b).hex()) for a, b in value))
        elif key in ('exit', 'fill', 'pred'):
            parts.append('%s=%s' % (key, value))
        else:
            parts.append('%s=%x' % (key, value))
    return ' '.join(parts)


def expectation(st, res, text):
    """The tokens of the registers the result changed."""
    out = {}
    if res['exit'] != 'fall':
        out['exit'] = res['exit']
        return out
    for i in range(31):
        if res['x'][i] != st['x'][i]:
            out['x%d' % i] = res['x'][i]
    for key in ('fpcr', 'fpsr'):
        if res[key] != st[key]:
            out[key] = res[key]
    for i in range(32):
        if res['z'][i] != st['z'][i]:
            out['z%d' % i] = res['z'][i]
    for i in range(16):
        if res['p'][i] != st['p'][i]:
            out['p%d' % i] = res['p'][i]
    for i, (a, b) in enumerate(zip(res['zarows'], st['zarows'])):
        if a != b:
            out['za%d' % i] = a
    if res['zt0'] != st['zt0']:
        out['zt0'] = res['zt0']
    if bool(res['sm']) != bool(st['sm']):
        out['sm'] = int(bool(res['sm']))
    if bool(res['za']) != bool(st['za']):
        out['za'] = int(bool(res['za']))
    if res['mem']:
        runs = []
        for a in sorted(res['mem']):
            if runs and runs[-1][0] + len(runs[-1][1]) == a:
                runs[-1][1].append(res['mem'][a])
            else:
                runs.append((a, [res['mem'][a]]))
        out['mem'] = runs
    return out


def entries():
    text = open(TABLE).read()
    out = []
    for m in re.finditer(r'\n    E\("([a-z0-9_]+)", (0x[0-9a-f]+), (0x[0-9a-f]+), (FEAT_\w+), ([A-Z_| 0-9]+), ', text):
        out.append((m.group(1), int(m.group(2), 16), int(m.group(3), 16), m.group(4), m.group(5)))
    return out


def disassemble(words):
    inp = ''.join('%08x\n' % w for w in words)
    res = subprocess.run([CLI, 'dis', '--arch', 'a64'], input=inp, capture_output=True, text=True).stdout.splitlines()
    return [line[5:].strip() if line.startswith('OK ') else None for line in res]


def shape(text):
    return re.sub(r'\b([xwzp]|pn)\d+\b', r'\1N', re.sub(r'0x[0-9a-f]+|\d+', 'N', text))


def fill_class(text):
    """The class of the elements of the registers of the instruction text."""
    mn = text.split()[0]
    suffixes = re.findall(r'\.([bhsd])\b', re.sub(r'\[[^\]]*\]', '', text))
    if mn.startswith(('f', 'bf')) and suffixes and not mn.startswith(('fcvtz',)):
        s = suffixes[-1]
        if s == 'h':
            return 'bf16' if mn.startswith('bf') else 'f16'
        if s in ('s', 'd'):
            return 'f32' if s == 's' else 'f64'
    return 'int'


def setup_for(text, index, flags):
    """The setup dictionary of a row."""
    seed = 1000 + index
    setup = {'fill': fill_class(text), 'seed': seed}
    if re.search(r'\bpn\d+\b', text):
        pass
    # the vector select and the slice registers
    for n in range(8, 16):
        setup['x%d' % n] = (seed + n) % 5
    if '[' in text and re.search(r'\[x\d+', text):
        # memory operands: bases into the data area, small indexes
        bases = [int(b) for b in re.findall(r'\[x(\d+)', text)]
        for k, b in enumerate(bases):
            setup['x%d' % b] = MEM_DATA + 0x40 * (k + 1)
        for idx in re.findall(r'\[x\d+, x(\d+)', text):
            setup['x%s' % idx] = 1
        data = bytes((seed * 31 + 7 * i + (i >> 3)) & 0xFF for i in range(1024))
        setup['mem'] = [(MEM_DATA, list(data))]
    # the floating point control: round to nearest, or another mode for some rows
    setup['fpcr'] = [0, 0x400000, 0x800000, 0xC00000, 0x1000000, 0x2000000, 0x4000000, 0x80000][index % 8]
    return setup


def main():
    ents = entries()
    # the sample words of every entry
    import random
    rng = random.Random(11)
    cand = []
    for i, (mn, mask, value, feat, flags) in enumerate(ents):
        for _ in range(8):
            cand.append((i, value | (rng.getrandbits(32) & ~mask & 0xFFFFFFFF)))
    texts = disassemble([w for _, w in cand])
    per = {}
    for (i, w), t in zip(cand, texts):
        if t is None or t.startswith('INVALID'):
            continue
        if re.search(r'\b[xw](18|28)\b', t):
            continue
        per.setdefault(i, [])
        if len(per[i]) < 2 and t not in [x[1] for x in per[i]]:
            per[i].append((w, t))
    # the entries dismount lifts: the instruction of the first sample doesn't end with OP_UNDEFINED
    order = sorted(per)
    inputs = []
    for i in order:
        st0 = make_state({})
        inputs.append(S.format_state('a64', per[i][0][0].to_bytes(4, 'little'), st0))
    res = subprocess.run([CLI, 'run', '--arch', 'a64'], input='\n'.join(inputs) + '\n',
                         capture_output=True, text=True).stdout.splitlines()
    lifted = set()
    for i, line in zip(order, res):
        if line.startswith('OK ') and not line.startswith('OK exit=undefined'):
            lifted.add(i)
    rows = []
    unsupported = {}
    seen_shapes = set()
    seen_mnemonics = set()
    for i, (mn, mask, value, feat, flags) in enumerate(ents):
        if i not in lifted:
            continue
        for w, t in per.get(i, [])[:1]:
            streaming = 'STREAMING_ONLY' in flags
            index = len(rows)
            # the row in the streaming mode with za enabled
            setup = setup_for(t, i, flags)
            st = make_state(setup)
            try:
                res = sme_model.run(t, st)
            except NotImplementedError as e:
                unsupported.setdefault(mn, []).append(t)
                continue
            except Exception as e:                       # a text the model can't parse
                unsupported.setdefault(mn, []).append('%s (%s)' % (t, e))
                continue
            key = (feat in M4_FEATURES, shape(t))
            on_m4 = feat in M4_FEATURES
            if not on_m4 or key not in seen_shapes:
                seen_shapes.add(key)
                rows.append((w, t, setup, expectation(st, res, t)))
            # the first form of every mnemonic at the other streaming vector lengths
            if mn not in seen_mnemonics:
                seen_mnemonics.add(mn)
                for svl in (256, 128):
                    setup3 = dict(setup)
                    setup3['svl'] = svl
                    st3 = make_state(setup3)
                    try:
                        res3 = sme_model.run(t, st3)
                    except Exception:
                        continue
                    rows.append((w, t, setup3, expectation(st3, res3, t)))
            # the instruction outside of its mode: not streaming (za enabled), and streaming without za
            uses_za = ('za' in t) or ('zt0' in t)
            for mode in ('nosm', 'noza'):
                if mode == 'noza' and not uses_za:
                    continue
                setup2 = {'sm': 0 if mode == 'nosm' else 1, 'za': 1 if mode == 'nosm' else 0}
                st2 = make_state(setup2)
                try:
                    res2 = sme_model.run(t, st2)
                except Exception:
                    continue
                if res2['exit'] == 'fall' and not on_m4:
                    # (ldr / str / zero of za and zt0 outside of the streaming mode): a row with data
                    pass
                rows.append((w, t, setup2, expectation(st2, res2, t)))
    # the mode changes (instructions of the base table): smstart, smstop, msr svcr, mrs svcr
    import armasm
    mode_texts = []
    for text in ('smstart', 'smstart sm', 'smstart za', 'smstop', 'smstop sm', 'smstop za'):
        for sm in (0, 1):
            for za in (0, 1):
                mode_texts.append((text, {'sm': sm, 'za': za, 'fill': 'int', 'seed': 77 + 4 * sm + 2 * za, 'x0': 0, 'fpsr': 3}))
    for value in (0, 1, 2, 3):
        for sm in (0, 1):
            for za in (0, 1):
                mode_texts.append(('msr svcr, x0', {'sm': sm, 'za': za, 'fill': 'int', 'seed': 90 + value, 'x0': value}))
    for sm in (0, 1):
        for za in (0, 1):
            mode_texts.append(('mrs x3, svcr', {'sm': sm, 'za': za, 'x3': 0x55}))
    # the vector length instructions (their encodings are in the SVE table): any mode, any svl
    for svl in (512, 256, 128):
        for text in ('rdsvl x0, #1', 'rdsvl x3, #-4', 'rdsvl x1, #31', 'addsvl x0, x1, #2', 'addsvl sp, sp, #-2',
                     'addspl x4, x5, #3', 'addspl sp, x2, #-32', 'addsvl x7, x7, #-32'):
            for sm in (0, 1):
                mode_texts.append((text, {'svl': svl, 'sm': sm, 'za': 0, 'x1': 0x1234, 'x2': 0x80, 'x5': 0x9000, 'x7': 5}))
    assembled = armasm.assemble('a64', [t for t, _ in mode_texts])
    for (text, setup), (hexb, err) in zip(mode_texts, assembled):
        assert err is None, (text, err)
        st = make_state(setup)
        res = sme_model.run(text, st)
        rows.append((int.from_bytes(bytes.fromhex(hexb), 'little'), text, setup, expectation(st, res, text)))
    with open(OUTPUT, 'w') as f:
        f.write('// Generated by tests/arm/tools/gen_sme_semantic_rows.py: the rows of tests/TestArm64SemanticsSme.cpp.\n')
        f.write('// { word, "text", "setup", "expected" }\n')
        f.write('#define ARM64_SME_ROWS \\\n')
        for w, t, setup, exp in rows:
            f.write('    { 0x%08x, "%s", "%s", "%s" }, \\\n' % (w, t, tokens(setup), tokens(exp)))
        f.write('\n')
    print('wrote %s: %d rows' % (OUTPUT, len(rows)))
    for mn, ts in sorted(unsupported.items()):
        print('  no model for %s: %s' % (mn, ts[0]))


if __name__ == '__main__':
    main()
