#!/usr/bin/env python3
"""Learn the encodings of a region of A64 from objdump: scan the words,
group them by mnemonic and print for each group the bits that never change
(mask, value) and whether they describe the group exactly (the number of
words is 2^(free bits)). The groups that are not exact have constraints
between the fields (an alias condition, a reserved value).

    learn.py 0x19000000 0x1a000000 [--step N] [--sample] [--min N]
"""
import argparse
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import armlib


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('start', type=lambda x: int(x, 0))
    parser.add_argument('end', type=lambda x: int(x, 0))
    parser.add_argument('--step', type=int, default=1)
    parser.add_argument('--sample', action='store_true', help='print two sample texts')
    parser.add_argument('--min', type=int, default=1, help='hide groups with fewer words')
    args = parser.parse_args()

    groups = {}
    chunk = 200000
    word = args.start
    while word < args.end:
        words = list(range(word, min(word + chunk * args.step, args.end), args.step))
        for w, text in zip(words, armlib.objdump_words(words)):
            if text == '<unknown>':
                continue
            m = text.split(' ')[0]
            g = groups.get(m)
            if g is None:
                groups[m] = [w, w, 1, [text]]       # and_bits, or_bits, count, samples
            else:
                g[0] &= w
                g[1] |= w
                g[2] += 1
                if len(g[3]) < 2:
                    g[3].append(text)
        word = words[-1] + args.step
    for m, (and_bits, or_bits, count, samples) in sorted(groups.items()):
        if count < args.min:
            continue
        varying = and_bits ^ or_bits
        mask = (~varying) & 0xffffffff
        free = bin(varying).count('1')
        exact = (count * args.step == (1 << free)) if args.step == 1 else None
        print('%-12s mask %08x value %08x count %-8d free %-2d %s' % (
            m, mask, and_bits & mask, count, free, 'exact' if exact else ('' if exact is None else 'NOT-exact')))
        if args.sample:
            for t in samples:
                print('      ' + t)


if __name__ == '__main__':
    main()
