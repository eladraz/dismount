#!/usr/bin/env python3
"""Random round trip fuzzer: dismount decoder -> dismount assembler -> decoder.

Every one of --count random byte strings is decoded by `dismount_cli dis`. The
text of each valid instruction is assembled by `dismount_cli asm` (the text the
disassembler printed, normalized like the REASM check of run_compat.py), and
the new bytes are decoded again. The assembler must

  (a) accept every text the disassembler produces, and
  (b) write bytes which decode to the same text and the same length.

The bytes themselves may differ from the random input (an extra prefix, a
non-canonical form: the assembler writes the canonical encoding), only the
decoded text counts.

Random bytes decode to texts which are no instruction (`lea rbx, r11`, `lock
lock add`, `mov dr1, [rax]`: the decoder is more lenient than the processors).
Nasm is the judge of such lines: an `ERR` of the assembler is a bug only if
nasm assembles the text, and a different text after the round trip is a bug
only if the bytes of the assembler are not the bytes of nasm for the text
(`mov r10, 0x74a651ac` is `mov r10d, ...` for both, `repne lock` is `lock
repne`). The other lines are counted as information. With --bits 64 the random bytes start with prefixes and a
REX prefix more often than not, so that the REX forms get their share.

Usage:
    python3 tests/ia32/tools/fuzz_roundtrip.py --bits 64 --count 100000 [--seed N]
    python3 tests/ia32/tools/fuzz_roundtrip.py --bits 32 --count 50000

Exit status 1 on a bug, 0 otherwise.
"""

import argparse
import os
import random
import re
import sys
import tempfile

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
TESTS_DIR = os.path.dirname(TOOLS_DIR)
sys.path.insert(0, TESTS_DIR)
import run_compat as rc  # noqa: E402

MAX_LEN = 15
# A branch, after the prefixes the disassembler writes in front of it
BRANCH_RE = re.compile(r'^((lock|rep|repne|repe|repz|repnz|[a-g]s) )*'
                       r'(j[a-z]*|call|loop[a-z]*) ')
PREFIXES = [0x66, 0x67, 0x2e, 0x36, 0x3e, 0x26, 0x64, 0x65, 0xf0, 0xf2, 0xf3]


PREFIX_WORDS = ('lock', 'rep', 'repne', 'repe', 'repz', 'repnz', 'es', 'cs', 'ss',
                'ds', 'fs', 'gs', 'a32', 'o16')


def canonical(text):
    """The text for the comparison: the order and the repetition of the
    prefixes are not an instruction difference, and nasm has no 'pextrw r64, mm' (the processor
    takes REX.W as the 32 bit form, the assembler writes eax)"""
    words = rc.norm_text(text).split(' ')
    prefixes = []
    while len(words) > 1 and words[0] in PREFIX_WORDS:
        prefixes.append(words.pop(0))
    rest = ' '.join(words)
    rest = re.sub(r'^pextrw [a-z0-9]+,', 'pextrw reg,', rest)
    # A repeated prefix is written once
    return ' '.join(sorted(set(prefixes)) + [rest])


def random_sequence(rng, bits):
    out = bytearray()
    # Legacy prefixes
    while rng.random() < 0.25 and len(out) < 3:
        out.append(rng.choice(PREFIXES))
    # A REX prefix in 64 bit mode
    if bits == 64 and rng.random() < 0.6:
        out.append(0x40 | rng.randrange(16))
    # The opcode: one byte, 0F xx, or x87
    r = rng.random()
    if r < 0.35:
        out.append(0x0f)
    out.append(rng.randrange(256))
    while len(out) < MAX_LEN:
        out.append(rng.randrange(256))
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--count', type=int, default=20000)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--bits', type=int, choices=(16, 32, 64), default=64)
    ap.add_argument('--cli', default=rc.DEFAULT_CLI)
    ap.add_argument('--show', type=int, default=30, help='samples to print per category')
    args = ap.parse_args()

    cli = os.path.abspath(args.cli)
    if not os.path.exists(cli):
        print('dismount_cli not found: %s (build with DISMOUNT_BUILD_TESTS=ON)' % cli)
        return 2

    rng = random.Random(args.seed)
    seqs = [random_sequence(rng, args.bits) for _ in range(args.count)]
    dec = rc.run_cli(cli, 'dis', args.bits, [s.hex() for s in seqs])

    items = []
    for data, line in zip(seqs, dec):
        m = re.match(r'^OK (\d+) ?(.*)$', line)
        if not m:
            continue
        length = int(m.group(1))
        text = m.group(2).rstrip()
        # The branches print an absolute target. 'rex' lines, prefixes alone
        # and the instructions with no text are not interesting
        if not text:
            continue
        items.append((data[:length].hex(), text))

    texts = [rc.reasm_text(t) for _, t in items]
    asm = rc.run_cli(cli, 'asm', args.bits, texts)
    again_in = []
    again_idx = []
    errors = []
    for i, out in enumerate(asm):
        if out.startswith('OK '):
            again_in.append(out[3:].strip())
            again_idx.append(i)
        else:
            errors.append((items[i][0], items[i][1], texts[i], out))
    again = rc.run_cli(cli, 'dis', args.bits, again_in)

    differ = []
    same_bytes = 0
    # (the lists hold (hexin, text, new_hex, got) and the index into items)
    differ_index = []
    for i, line in zip(again_idx, again):
        m = re.match(r'^(OK|INVALID) (\d+) ?(.*)$', line)
        hexin, text = items[i]
        new_hex = asm[i][3:].strip()
        if not m or m.group(1) != 'OK' or int(m.group(2)) * 2 != len(new_hex):
            differ.append((hexin, text, new_hex, line))
            differ_index.append(i)
            continue
        if new_hex == hexin:
            same_bytes += 1
            continue
        if canonical(m.group(3).rstrip()) != canonical(text):
            differ.append((hexin, text, new_hex, m.group(3).rstrip()))
            differ_index.append(i)

    # Nasm is the judge of the lines that differ
    with tempfile.TemporaryDirectory() as tmp:
        err_nasm = rc.nasm_assemble('nasm', [e[2] for e in errors], args.bits, tmp) \
            if errors else []
        diff_nasm = rc.nasm_assemble('nasm', [texts[i] for i in differ_index],
                                     args.bits, tmp) if differ else []
    bug_errors = []
    nasm_rejects = 0
    for e, (hexb, err) in zip(errors, err_nasm):
        if err is not None:
            nasm_rejects += 1
        elif 'used twice' in e[3]:
            # A repeated prefix is rejected by design (asm_errors.txt), nasm
            # writes it once
            nasm_rejects += 1
        else:
            bug_errors.append(e + (hexb,))
    bug_differ = []
    nasm_same = 0
    for d, i, (hexb, err) in zip(differ, differ_index, diff_nasm):
        if err is None and hexb == d[2]:
            nasm_same += 1
        elif BRANCH_RE.search(d[1]) and len(d[2]) != len(d[0]):
            # The target of a branch is relative to the start of the
            # instruction: a redundant prefix in the random bytes moves it
            nasm_same += 1
        else:
            bug_differ.append(d + ('nasm: ' + (hexb if err is None else err),))

    print('fuzz_roundtrip: %d sequences, seed %d, %d-bit: %d decoded, %d bytes identical'
          % (args.count, args.seed, args.bits, len(items), same_bytes))
    print('  info: text rejected by nasm too or a repeated prefix: %d; different text after the round trip '
          'but the bytes of nasm: %d' % (nasm_rejects, nasm_same))
    print('(a) text accepted by nasm and not by the assembler: %d' % len(bug_errors))
    for hexin, text, norm, out, nasm_hex in bug_errors[:args.show]:
        print('    %s  "%s"  -> %s  (nasm %s)' % (hexin, norm, out[:100], nasm_hex))
    print('(b) different text after the round trip, bytes not those of nasm: %d'
          % len(bug_differ))
    for hexin, text, new_hex, got, nasm_out in bug_differ[:args.show]:
        print('    %s "%s"  -> %s "%s"  (%s)' % (hexin, text, new_hex, got, nasm_out))
    failed = len(bug_errors) + len(bug_differ)
    print('RESULT: %s' % ('FAIL' if failed else 'OK'))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
