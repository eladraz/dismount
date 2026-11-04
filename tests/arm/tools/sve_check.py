#!/usr/bin/env python3
"""Check the fixed bits of the SVE entries of gen_a64_sve_table.py against
objdump, before the C++ is built.

  precision  for every entry, random words with its fixed bits through objdump:
             the mnemonic objdump prints has to be the entry's one (or an alias
             of it, or <unknown> for a reserved combination of the fields).
  recall     random words of the SVE class which objdump decodes have to match
             the fixed bits of some entry.

    gen_a64_sve_table.py --check [--samples N] [--words N] [--seed S] [--only MNEMONIC]
"""
import argparse
import collections
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import armlib
import a64dsl_sve


def sve_top_bytes():
    return [0x04, 0x05, 0x24, 0x25, 0x44, 0x45, 0x64, 0x65, 0x84, 0x85, 0xa4, 0xa5,
            0xc4, 0xc5, 0xe4, 0xe5]


def precision(args, rng):
    entries = [e for e in a64dsl_sve.ENTRIES
               if not args.only or e.mnemonic == args.only]
    words = []
    owners = []
    for index, entry in enumerate(entries):
        free = ~entry.mask & 0xffffffff
        for _ in range(args.samples):
            words.append(entry.value | (rng.getrandbits(32) & free))
            owners.append(index)
    texts, _ = armlib.objdump_chunks(words)
    stats = [collections.Counter() for _ in entries]
    samples = [{} for _ in entries]
    for word, owner, text in zip(words, owners, texts):
        mnemonic = text.split(' ')[0]
        stats[owner][mnemonic] += 1
        samples[owner].setdefault(mnemonic, (word, text))
    bad = 0
    for index, entry in enumerate(entries):
        counter = stats[index]
        known = {m: c for m, c in counter.items() if m != '<unknown>'}
        if entry.mnemonic in known:
            # Other mnemonics are aliases or the neighbours the fields select
            continue
        if not known:
            # Only reserved combinations: fine for a rare entry, suspicious else
            if args.samples >= 8:
                print('NEVER DECODES  %-10s %s  (%d unknown)' % (
                    entry.mnemonic, entry.pattern, counter['<unknown>']))
                bad += 1
            continue
        bad += 1
        print('WRONG MNEMONIC %-10s %s' % (entry.mnemonic, entry.pattern))
        for m, c in sorted(known.items(), key=lambda kv: -kv[1])[:4]:
            word, text = samples[index][m]
            print('      %-10s x%-3d e.g. %08x  %s' % (m, c, word, text))
    print('precision: %d entries, %d suspicious' % (len(entries), bad))
    return bad


def recall(args, rng):
    buckets = collections.defaultdict(list)
    for e in a64dsl_sve.ENTRIES:
        for top in range(256):
            if (top & (e.mask >> 24)) == ((e.value & e.mask) >> 24):
                buckets[top].append(e)
    words = []
    tops = sve_top_bytes()
    per = args.words // len(tops)
    for top in tops:
        for _ in range(per):
            words.append((top << 24) | rng.getrandbits(24))
    texts, _ = armlib.objdump_chunks(words)
    missing = collections.defaultdict(list)
    count = 0
    for word, text in zip(words, texts):
        if text == '<unknown>':
            continue
        count += 1
        if any((word & e.mask) == (e.value & e.mask) for e in buckets[word >> 24]):
            continue
        missing[text.split(' ')[0]].append((word, text))
    total = sum(len(v) for v in missing.values())
    for mnemonic, items in sorted(missing.items(), key=lambda kv: -len(kv[1])):
        print('NOT COVERED %-10s x%-5d e.g. %08x  %s' % (mnemonic, len(items), items[0][0], items[0][1]))
    print('recall: %d decodable words, %d not covered (%d mnemonics)' % (count, total, len(missing)))
    return total


def main(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument('--samples', type=int, default=24)
    parser.add_argument('--words', type=int, default=160000)
    parser.add_argument('--seed', type=int, default=3)
    parser.add_argument('--only')
    parser.add_argument('--no-recall', action='store_true')
    parser.add_argument('--no-precision', action='store_true')
    args = parser.parse_args(argv)
    rng = armlib.seeded_rng(args.seed)
    bad = 0
    if not args.no_precision:
        bad += precision(args, rng)
    if not args.no_recall:
        bad += recall(args, rng)
    return 1 if bad else 0
