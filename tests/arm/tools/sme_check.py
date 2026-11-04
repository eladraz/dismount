#!/usr/bin/env python3
"""Check the fixed bits of the SME entries of gen_a64_sme_table.py against
objdump, before the C++ is built.

  precision  for every entry, random words with its fixed bits through objdump:
             the mnemonic objdump prints has to be the entry's one (or an alias
             of it, or <unknown> for a reserved combination of the fields).
  recall     the words of the SME class which objdump decodes (the top bytes
             with random low bits, and the neighbours of every entry: the fixed
             bits flipped) have to match the fixed bits of some entry.

    gen_a64_sme_table.py --check [--samples N] [--words N] [--seed S] [--only MNEMONIC]
"""
import argparse
import collections
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import armlib
import a64dsl_sme

TOPS = [0x80, 0x81, 0xa0, 0xa1, 0xc0, 0xc1, 0xe0, 0xe1]
# objdump prints mova as mov
PRINTED = {'mova': 'mov'}


def printed(mnemonic):
    return PRINTED.get(mnemonic, mnemonic)


def precision(args, rng):
    entries = [e for e in a64dsl_sme.ENTRIES if not args.only or e.mnemonic == args.only]
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
        if printed(entry.mnemonic) in known:
            continue
        if not known:
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
    for e in a64dsl_sme.ENTRIES:
        for top in range(256):
            if (top & (e.mask >> 24)) == ((e.value & e.mask) >> 24):
                buckets[top].append(e)
    words = []
    per = args.words // len(TOPS)
    for top in TOPS:
        for _ in range(per):
            words.append((top << 24) | rng.getrandbits(24))
    # The neighbours of the entries: one or two fixed bits flipped
    for e in a64dsl_sme.ENTRIES:
        free = ~e.mask & 0xffffffff
        for _ in range(args.neighbours):
            w = e.value | (rng.getrandbits(32) & free)
            for _ in range(rng.choice((1, 1, 2))):
                w ^= 1 << rng.randrange(32)
            if armlib.a64_class(w) == 'sme':
                words.append(w)
    texts, _ = armlib.objdump_chunks(words)
    missing = collections.defaultdict(list)
    count = 0
    for word, text in zip(words, texts):
        if text == '<unknown>':
            continue
        count += 1
        if any((word & e.mask) == (e.value & e.mask) for e in buckets[word >> 24]):
            continue
        # (the multi vector loads and stores of SVE2.1 are in the SVE table)
        if re.match(r'(ld|st)(nt)?1[bhwd] ', text) and 'pn' in text:
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
    parser.add_argument('--words', type=int, default=800000)
    parser.add_argument('--neighbours', type=int, default=24)
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
