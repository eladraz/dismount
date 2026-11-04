#!/usr/bin/env python3
"""Scan the system instruction space of A64 (0xd5000000-0xd53fffff) through
objdump, to a file of "word text" lines. The ground truth of the system
instructions: the names of the options and of the system registers, and
the exhaustive comparison with dismount (tests/arm/tools/check_system.py).

    scan_system.py OUT [--step N]     # N > 1 scans every Nth word
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import armlib


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('output')
    parser.add_argument('--start', type=lambda x: int(x, 16), default=0xd5000000)
    parser.add_argument('--end', type=lambda x: int(x, 16), default=0xd5400000)
    parser.add_argument('--step', type=int, default=1)
    args = parser.parse_args()

    chunk = 200000
    with open(args.output, 'w') as out:
        word = args.start
        while word < args.end:
            words = list(range(word, min(word + chunk * args.step, args.end), args.step))
            texts = armlib.objdump_words(words)
            for w, text in zip(words, texts):
                out.write('%08x %s\n' % (w, text))
            word = words[-1] + args.step
            armlib.log('%08x' % word)


if __name__ == '__main__':
    main()
