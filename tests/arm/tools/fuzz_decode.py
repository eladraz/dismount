#!/usr/bin/env python3
"""Random words through objdump and dismount (tests/arm/TEST_PLAN.md).

A word is generated three ways: fully random, from an encoding of the dismount
tables (its fixed bits, random operand bits, and sometimes a flipped fixed
bit, so the neighbours of an encoding are reached) and from a random top byte
(the classes of the architecture whose tables are sparse).

A bug is: a different text, dismount decodes and objdump says <unknown>, the
reverse, or a crash. Words of the classes that dismount doesn't implement yet
(the scalar floating point and Advanced SIMD, SVE and SME) are counted as
"unimplemented", not as bugs.

    fuzz_decode.py --arch a64 --count 200000 --seed 1
"""
import argparse
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import armlib


def generate(arch, count, seed, rows):
    rng = armlib.seeded_rng(seed)
    words = set()
    entries = [r for r in rows if r[4] == 4]
    while len(words) < count:
        kind = rng.random()
        if kind < 0.25:
            word = rng.getrandbits(32)
        elif kind < 0.85 and entries:
            mask, value = rng.choice(entries)[:2]
            word = value | (rng.getrandbits(32) & ~mask & 0xffffffff)
            # Flip a fixed bit now and then: the neighbours of the encoding
            if rng.random() < 0.3:
                for _ in range(rng.choice((1, 1, 2))):
                    word ^= 1 << rng.randrange(32)
        else:
            word = (rng.getrandbits(8) << 24) | rng.getrandbits(24)
        words.add(word & 0xffffffff)
    return sorted(words)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--arch', default='a64', choices=['a64'])
    parser.add_argument('--count', type=int, default=20000)
    parser.add_argument('--seed', type=int, default=1)
    parser.add_argument('--show', type=int, default=3,
                        help='examples per group of mismatches')
    parser.add_argument('--words', help='file of hex words to test instead of random ones')
    args = parser.parse_args()

    rows = armlib.entries(args.arch)
    if args.words:
        words = [int(w, 16) for w in open(args.words).read().split()]
    else:
        words = generate(args.arch, args.count, args.seed, rows)

    return compare_and_report(args.arch, words, args.show)


def compare_and_report(arch, words, show=3, only_class=None):
    """Compare the words through objdump and dismount, print the report.
    Return the exit code (1 if there is a mismatch)."""
    objdump, addresses = armlib.objdump_chunks(words, arch)
    # The dismount side: the same addresses
    ours = armlib.dis_words(arch, words, None, addresses)

    stats = collections.Counter()
    groups = collections.defaultdict(list)
    for word, address, theirs, (status, text) in zip(words, addresses, objdump, ours):
        cls = armlib.a64_class(word)
        if cls != 'base' and only_class is None:
            stats['unimplemented (%s)' % cls] += 1
            continue
        stats['compared'] += 1
        unknown = (theirs == '<unknown>')
        if status in ('CRASH', 'ERR'):
            groups[('%s' % status, armlib.mnemonic_of(theirs))].append((word, theirs, status))
        elif unknown and status == 'INVALID':
            stats['both invalid'] += 1
        elif not unknown and status == 'OK' and text == theirs:
            stats['same text'] += 1
        elif unknown:
            groups[('EXTRA', armlib.mnemonic_of(text))].append((word, theirs, text))
        elif status == 'INVALID':
            groups[('MISSING', armlib.mnemonic_of(theirs))].append((word, theirs, 'INVALID'))
        else:
            groups[('DIFF', armlib.mnemonic_of(theirs))].append((word, theirs, text))

    bugs = sum(len(v) for v in groups.values())
    for key in sorted(stats):
        print('%-28s %d' % (key, stats[key]))
    print('%-28s %d' % ('mismatches', bugs))
    for (kind, mnemonic), items in sorted(groups.items(), key=lambda kv: -len(kv[1])):
        print('\n%s %s (%d)' % (kind, mnemonic, len(items)))
        for word, theirs, ours_text in items[:show]:
            print('   %08x  objdump: %s' % (word, theirs))
            print('             dismount: %s' % ours_text)
    return 1 if bugs else 0


if __name__ == '__main__':
    sys.exit(main())
