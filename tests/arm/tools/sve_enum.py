#!/usr/bin/env python3
"""Enumerate the values of some bit fields of a word through objdump: the way to
learn what a field selects.

    sve_enum.py 0x65a0a000 23-22,18-16 [--show-unknown]

The word gives the other bits (the register numbers); the fields are written
high-low and separated by commas, they are enumerated together (the label is
the value of the concatenated bits).
"""
import argparse
import itertools
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import armlib


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('word', type=lambda x: int(x, 0))
    parser.add_argument('fields')
    parser.add_argument('--show-unknown', action='store_true')
    args = parser.parse_args()
    ranges = []
    for part in args.fields.split(','):
        hi, _, lo = part.partition('-')
        hi = int(hi)
        lo = int(lo) if lo else hi
        ranges.append((hi, lo))
    total = sum(h - l + 1 for h, l in ranges)
    words = []
    for value in range(1 << total):
        word = args.word
        shift = total
        for hi, lo in ranges:
            width = hi - lo + 1
            shift -= width
            field = (value >> shift) & ((1 << width) - 1)
            word = (word & ~(((1 << width) - 1) << lo)) | (field << lo)
        words.append((value, word))
    texts, _ = armlib.objdump_chunks([w for _, w in words])
    for (value, word), text in zip(words, texts):
        if text == '<unknown>' and not args.show_unknown:
            continue
        print('%0*s  %08x  %s' % (total, bin(value)[2:].zfill(total), word, text))


if __name__ == '__main__':
    main()
