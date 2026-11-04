#!/usr/bin/env python3
"""
Random encodings of AArch32 against Unicorn: the corpus (corpus/a32_*.s, t32_*.s) holds the
instructions as the assembler spells them, this driver runs the instruction words that clang
would never emit (every combination of the register fields, the pc / sp / lr operands, the odd
shift amounts, the unusual conditions) through the same comparison as run_semantics.py.

    out/venv/bin/python tests/arm/semantics/fuzz_aarch32.py --arch a32|t32 [--count 20000]
        [--seed N] [--states 4] [--grep REGEX] [--simd] [--out DIR]

A random word is kept when dismount decodes it (`dismount_cli dis`), the decoded text does not
match skip.txt, and (unless --simd) it is not a VFP / Advanced SIMD / cryptographic instruction.
The text of the instruction gives the states (memory operands point into the window, see
states_arm.py). The lines whose mnemonic has no semantics (UNLIFTED) are counted, not failed.
Exit status 1 when a line FAILs; the failing lines are written to DIR/failures.txt.
"""
import argparse
import os
import random
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import armasm
import run_semantics as R

SIMD = re.compile(r'^(v[a-z]|aes|sha|fld|fst)')
# The register combinations the Arm ARM calls UNPREDICTABLE although the encoding exists: the pc as
# the base register of a writeback, of a multiple access or of an unprivileged access
UNPREDICTABLE = re.compile(r'\[pc[^\]]*\]!|\[pc\],|^(?:ld|st)m[\w.]*\s+pc\b|^(?:ldr|str)\w*t\w*\s+[^,]+,\s*\[pc|'
                           r'^(?:ld[ar]|stl|[sl]\w*ex)\w*\s.*\[pc|^(?:strex|stlex)\w*\s+pc\b')
# ... and the pc as an operand of the instructions which don't take it (multiplies, saturating and
# parallel arithmetic, bit fields, extends, reverses, crc32, movw / movt)
NO_PC = re.compile(r'^(?:mul|mla|mls|[us]mul|[us]mla|[us]mlal|umaal|smm|smu|sml|usad|[su]div|q[du]?add|q[du]?sub|'
                   r'[su]sat|bfi|bfc|[su]bfx|rev|rbit|clz|crc32|sel|pkh|[su]xt|[su]h?q?(?:add|sub|asx|sax)|'
                   r'uq(?:add|sub|asx|sax)|movw|movt)\w*\s.*\bpc\b')


def random_words(arch, count, rng):
    """(hex bytes in memory order, hex for `dis`) of random instruction words."""
    out = []
    for _ in range(count):
        if arch == 'a32':
            word = rng.getrandbits(32)
            if rng.random() < 0.8:
                word = (word & 0x0FFFFFFF) | (0xE << 28)
            out.append((word.to_bytes(4, 'little').hex(), '%08x' % word))
        else:
            if rng.random() < 0.4:
                half = rng.getrandbits(16)
                out.append((half.to_bytes(2, 'little').hex(), '%04x' % half))
            else:
                first = (0x1D + rng.getrandbits(2) % 3) << 11 | rng.getrandbits(11)
                second = rng.getrandbits(16)
                out.append((first.to_bytes(2, 'little').hex() + second.to_bytes(2, 'little').hex(),
                            '%04x %04x' % (first, second)))
    return out


def disassemble(arch, words):
    proc = subprocess.run([R.DEFAULT_CLI, 'dis', '--arch', arch], input=''.join(w[1] + '\n' for w in words),
                          stdout=subprocess.PIPE, universal_newlines=True)
    return proc.stdout.splitlines()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[1])
    ap.add_argument('--arch', choices=('a32', 't32'), required=True)
    ap.add_argument('--count', type=int, default=20000)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--states', type=int, default=4)
    ap.add_argument('--grep', default='')
    ap.add_argument('--simd', action='store_true')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 1)
    ap.add_argument('--out', default=os.path.join(R.DEFAULT_OUT, '..', 'fuzz_aarch32'))
    ap.add_argument('--cli', default=R.DEFAULT_CLI)
    args = ap.parse_args()
    rng = random.Random(args.seed)
    R.lazy_oracles()
    skips = R.load_rules(R.SKIP_FILE)
    ctx = {'cli': os.path.abspath(args.cli), 'overrides': {}, 'oracle_bugs': R.load_rules(R.ORACLE_BUGS_FILE)}
    pattern = re.compile(args.grep) if args.grep else None

    words = random_words(args.arch, args.count, rng)
    lines = disassemble(args.arch, words)
    recs = []
    seen = set()
    for (hexbytes, _), line in zip(words, lines):
        if not line.startswith('OK '):
            continue
        text = line.split(' ', 2)[2].strip().lower()
        mnemonic = text.split()[0] if text.split() else ''
        if hexbytes in seen or (pattern and not pattern.search(text)):
            continue
        # (an it opens a block in the assembler: the next lines would be inside it)
        if re.match(r'it[te]{0,3}$', mnemonic):
            continue
        if (not args.simd and SIMD.match(mnemonic)) or UNPREDICTABLE.search(text) or NO_PC.search(text):
            continue
        # A store exclusive whose status register is also a source or the base is UNPREDICTABLE
        if re.match(r'(?:strex|stlex)', mnemonic):
            registers = re.findall(r'\b(r\d+|sp|lr|pc)\b', text)
            if registers[0] in registers[1:]:
                continue
        if any((arches is None or args.arch in arches) and rx.search(text) for rx, reason, arches in skips):
            continue
        seen.add(hexbytes)
        recs.append({'file': 'fuzz', 'line': len(recs) + 1, 'source': text, 'arch': args.arch, 'mode': 'plain',
                     'hex': hexbytes, 'feature': '', 'fixed': [], 'mnemonic': mnemonic, 'status': None,
                     'message': '', 'diffs': []})
    # Only the canonical encodings: the text must assemble back to the same bytes (the words with
    # the SBZ / SBO fields wrong, the UNPREDICTABLE register combinations, the non existing
    # spellings are decoded by dismount like LLVM does, and executed in a way nobody defines)
    assembled = armasm.assemble(args.arch, [r['source'] for r in recs])
    recs = [r for r, (hexb, err) in zip(recs, assembled) if err is None and hexb == r['hex']]
    for number, r in enumerate(recs, 1):
        r['line'] = number
    sys.stderr.write('  %d random words, %d canonical instructions\n' % (len(words), len(recs)))
    ns = argparse.Namespace(arch=args.arch, oracle='unicorn', states=args.states, oracle_only=False, jobs=args.jobs,
                            vl=128, compare_oracles=False)
    R.process(recs, ns, ctx)
    totals, fails = R.write_reports(recs, os.path.abspath(args.out), ns, 'AArch32 fuzz (%s, seed %d)' % (args.arch, args.seed))
    print('fuzz %s seed %d: %d instructions: %s' % (args.arch, args.seed, len(recs),
                                                  '  '.join('%s=%d' % (s, totals.get(s, 0)) for s in R.STATUSES)))
    if fails:
        print('FAIL mnemonics (%d): %s' % (len(fails), ' '.join(fails[:80])))
    print('reports: %s' % os.path.abspath(args.out))
    return 1 if totals.get('FAIL', 0) else 0


if __name__ == '__main__':
    sys.exit(main())
