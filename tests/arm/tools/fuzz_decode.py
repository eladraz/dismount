#!/usr/bin/env python3
"""Random words through objdump and dismount (tests/arm/TEST_PLAN.md).

A word is generated three ways: fully random, from an encoding of the dismount
tables (its fixed bits, random operand bits, and sometimes a flipped fixed
bit, so the neighbours of an encoding are reached) and from a random top byte
(the classes of the architecture whose tables are sparse).

A bug is: a different text, dismount decodes and objdump says <unknown>, the
reverse, or a crash. Words of the classes that dismount doesn't implement yet
(none now) are counted as "unimplemented", not as bugs.

    fuzz_decode.py --arch a64 --count 200000 --seed 1
"""
import argparse
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import armlib


# The classes whose words are compared (the others are not implemented yet)
IMPLEMENTED_CLASSES = ('base', 'simd', 'sve', 'sme')


def generate(arch, count, seed, rows, only_class=None, tops=None):
    """Random words. only_class restricts them to an encoding class
    (armlib.a64_class): 'simd' reaches the scalar floating point and Advanced
    SIMD space, which is sparse, through the entries of that class."""
    rng = armlib.seeded_rng(seed)
    words = set()
    entries = [r for r in rows if r[4] == 4]
    if only_class:
        entries = [r for r in entries if armlib.a64_class(r[1]) == only_class]
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
        elif tops:
            word = (rng.choice(tops) << 24) | rng.getrandbits(24)
        else:
            word = (rng.getrandbits(8) << 24) | rng.getrandbits(24)
        word &= 0xffffffff
        if only_class == 'sve':
            # The SVE class is the bits 28-25 = 0010: not rare, but sparse
            word = (word & ~0x1e000000) | 0x04000000
        if only_class == 'sme':
            # The SME class is the bits 28-25 = 0000 with bit 31 set
            word = (word & ~0x1e000000) | 0x80000000
        if only_class and armlib.a64_class(word) != only_class:
            continue
        words.add(word)
    return sorted(words)


def generate_t32(count, seed, rows):
    """Random T32 units: random halfwords, the fixed bits of the entries with
    random operand bits (and a flipped bit now and then). An it instruction is
    replaced by a nop: the it state of a stream would carry to the next units."""
    rng = armlib.seeded_rng(seed)
    units = set()
    wide = [r for r in rows if r[4] == 4]
    narrow = [r for r in rows if r[4] == 2]
    while len(units) < count:
        kind = rng.random()
        if kind < 0.2:
            value = rng.getrandbits(32)
        elif kind < 0.85 and rows:
            mask, value_, _, _, size, _ = rng.choice(wide if rng.random() < 0.6 and wide else narrow)
            value = (value_ | (rng.getrandbits(32) & ~mask & 0xffffffff)) & (0xffffffff if size == 4 else 0xffff)
            if size == 2:
                value <<= 16
            if rng.random() < 0.3:
                for _ in range(rng.choice((1, 1, 2))):
                    value ^= 1 << rng.randrange(16 if size == 2 else 32, 32) if size == 2 else 1 << rng.randrange(32)
        else:
            value = (rng.getrandbits(16) << 16) | rng.getrandbits(16)
        unit = armlib.t32_unit(value & 0xffffffff)
        if armlib.t32_is_it(unit):
            unit = (2, 0xbf00)
        units.add(unit)
    return sorted(units)


def compare_units(units, show=3):
    objdump, addresses = armlib.objdump_unit_chunks(units)
    ours = armlib.dis_units(units, None, addresses)
    stats = collections.Counter()
    groups = collections.defaultdict(list)
    for unit, theirs, (status, text) in zip(units, objdump, ours):
        stats['compared'] += 1
        unknown = (theirs == '<unknown>')
        word = armlib.t32_hex(unit)
        if status in ('CRASH', 'ERR'):
            groups[(status, armlib.mnemonic_of(theirs))].append((word, theirs, status))
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
            print('   %-10s  objdump: %s' % (word, theirs))
            print('              dismount: %s' % ours_text)
    return 1 if bugs else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--arch', default='a64', choices=['a64', 'a32', 't32'])
    parser.add_argument('--count', type=int, default=20000)
    parser.add_argument('--seed', type=int, default=1)
    parser.add_argument('--show', type=int, default=3,
                        help='examples per group of mismatches')
    parser.add_argument('--words', help='file of hex words to test instead of random ones')
    parser.add_argument('--class', dest='only_class', choices=['base', 'simd', 'sve', 'sme'],
                        help='only the words of this encoding class (armlib.a64_class)')
    parser.add_argument('--tops', help='comma separated top bytes (hex): the random words '
                                       'are made of one of them and random low bits')
    args = parser.parse_args()

    rows = armlib.entries(args.arch)
    if args.arch == 't32':
        return compare_units(generate_t32(args.count, args.seed, rows), args.show)
    if args.words:
        words = [int(w, 16) for w in open(args.words).read().split()]
    else:
        tops = [int(t, 16) for t in args.tops.split(',')] if args.tops else None
        words = generate(args.arch, args.count, args.seed, rows, args.only_class, tops)

    return compare_and_report(args.arch, words, args.show, args.only_class)


def compare_and_report(arch, words, show=3, only_class=None):
    """Compare the words through objdump and dismount, print the report.
    Return the exit code (1 if there is a mismatch)."""
    objdump, addresses = armlib.objdump_chunks(words, arch)
    # The dismount side: the same addresses
    ours = armlib.dis_words(arch, words, None, addresses)

    stats = collections.Counter()
    groups = collections.defaultdict(list)
    for word, address, theirs, (status, text) in zip(words, addresses, objdump, ours):
        cls = armlib.a64_class(word) if arch == 'a64' else 'base'
        if cls not in IMPLEMENTED_CLASSES and only_class is None:
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
